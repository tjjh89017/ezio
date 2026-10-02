#include "app.hpp"

#include "spdlog/spdlog.h"

#include <libtorrent/libtorrent.hpp>

#include <libtorrent/extensions/ut_pex.hpp>

#include "raw_disk_io.hpp"

namespace
{
// Session-wide peer cap, high enough that it never binds.
constexpr int SESSION_PEER_LIMIT = 1000;
}  // namespace

namespace ezio
{

// static
lt::session_params app::make_session_params(const config &cfg)
{
	lt::settings_pack p;
	// setup alert mask
	p.set_int(lt::settings_pack::alert_mask,
		lt::alert_category::error | lt::alert_category::status | lt::alert_category::peer);

	// disable all encrypt to avoid bug https://github.com/arvidn/libtorrent/issues/6735#issuecomment-1036675263
	p.set_int(lt::settings_pack::out_enc_policy, lt::settings_pack::pe_disabled);
	p.set_int(lt::settings_pack::in_enc_policy, lt::settings_pack::pe_disabled);

	// disable uTP for better performance
	p.set_bool(lt::settings_pack::enable_outgoing_utp, false);
	p.set_bool(lt::settings_pack::enable_incoming_utp, false);
	p.set_int(lt::settings_pack::mixed_mode_algorithm, lt::settings_pack::prefer_tcp);

	// DHT / LSD are disabled by default; enable via --enable-dht / --enable-lsd.
	// PEX is always on (see the app ctor).
	p.set_bool(lt::settings_pack::enable_dht, cfg.dht);
	p.set_bool(lt::settings_pack::enable_lsd, cfg.lsd);

	// Single-host tests run every peer on 127.0.0.1; libtorrent keeps only
	// one connection per IP unless this is set.
	p.set_bool(lt::settings_pack::allow_multiple_connections_per_ip, cfg.multi_conn_per_ip);

	// LAN deployment: no NAT traversal needed, so disable UPnP and NAT-PMP.
	p.set_bool(lt::settings_pack::enable_upnp, false);
	p.set_bool(lt::settings_pack::enable_natpmp, false);

	// thread pool size from config (used for both I/O and hashing)
	p.set_int(lt::settings_pack::aio_threads, cfg.aio_threads);
	p.set_int(lt::settings_pack::hashing_threads, cfg.aio_threads);
	spdlog::info("Thread pool: aio_threads={} (used for both I/O and hashing)", cfg.aio_threads);

	// Piece suggestions made deployment slower when the link limits; keep them off.
	p.set_int(lt::settings_pack::suggest_mode, lt::settings_pack::no_piece_suggestions);

	// network buffer sizes
	p.set_int(lt::settings_pack::max_queued_disk_bytes, 128 * 1024 * 1024);
	p.set_int(lt::settings_pack::send_not_sent_low_watermark, 524288);
	// Small send chain: every send walks the whole chain (chained_buffer::build_vec).
	p.set_int(lt::settings_pack::send_buffer_watermark, 2 * 1024 * 1024);
	p.set_int(lt::settings_pack::send_buffer_low_watermark, 512 * 1024);

	// Keep the session-wide caps out of the way; the effective limits are enforced
	// per-torrent (max_connections / max_uploads, set in add_torrent). Peers are
	// routed into the global peer class (see set_peer_class_filter in the app
	// ctor) so the session-wide upload_rate_limit the slow-start ramp sets
	// reaches LAN peers too -- libtorrent's default would otherwise exempt them
	// via a local class.
	// unchoke_slots_limit must be positive: with -1 libtorrent skips the unchoke
	// round, and a freed upload slot is never given to a peer that waits.
	p.set_int(lt::settings_pack::connections_limit, SESSION_PEER_LIMIT);
	p.set_int(lt::settings_pack::unchoke_slots_limit, SESSION_PEER_LIMIT);

	// Close a connection after 60 s with no interest in both directions
	// (libtorrent default: 600), so that peers with the same pieces dial others.
	p.set_int(lt::settings_pack::inactivity_timeout, 60);

	// unified_cache size from config (default 512MB)
	// Note: cache_size is deprecated but still used by raw_disk_io
	// cache_size unit: number of 16KiB blocks
	// Convert MB to number of 16KB blocks: (MB * 1024) / 16
	int cache_blocks = (cfg.cache_size_mb * 1024) / 16;
	p.set_int(lt::settings_pack::cache_size, cache_blocks);
	spdlog::info("Cache size: {} MB ({} blocks)", cfg.cache_size_mb, cache_blocks);

	// BitTorrent peer listen port. Default bt_listen_port == 0 leaves
	// libtorrent's listen_interfaces untouched (behavior unchanged); a
	// non-zero value binds the BT peer port on every interface (IPv4 + IPv6).
	if (cfg.bt_listen_port != 0) {
		std::string listen_ifaces = "0.0.0.0:" + std::to_string(cfg.bt_listen_port) + ",[::]:" + std::to_string(cfg.bt_listen_port);
		p.set_str(lt::settings_pack::listen_interfaces, listen_ifaces);
		spdlog::info("BitTorrent listen port: {}", cfg.bt_listen_port);
	}

	// Construct with an empty extension list instead of libtorrent's
	// default_plugins() (ut_pex, ut_metadata, smart_ban) so ut_metadata and
	// smart_ban stay off (smart_ban is deliberately disabled); ut_pex is added
	// back unconditionally in the app ctor via session::add_extension. Note
	// libtorrent skips ut_pex automatically for private torrents.
	lt::session_params ses_params(p, {});
	if (!cfg.file_flag) {
		ses_params.disk_io_constructor = raw_disk_io_constructor;
	}
	return ses_params;
}

app::app(const config &cfg) : m_config(cfg), m_session(make_session_params(cfg)), m_daemon(m_session, m_config.slow_start, m_config.slow_start_period), m_service(m_daemon), m_log(m_daemon, m_daemon.get_io_context())
{
	// PEX is always on (session_params was built with an empty extension list
	// to keep ut_metadata and smart_ban off, so ut_pex is added back here
	// unconditionally). libtorrent skips ut_pex automatically for private
	// torrents.
	m_session.add_extension(&lt::create_ut_pex_plugin);

	// Route every peer -- including LAN/private addresses -- into the global peer
	// class. By default libtorrent maps private-range IPs to a separate local
	// peer class that ignores the session-wide upload_rate_limit (and the unchoke
	// slot limits). Putting everyone in the global class makes the slow-start
	// ramp's rate limit apply uniformly and lets the per-torrent max_uploads
	// govern the unchoke count. This is the non-deprecated equivalent of
	// ignore_limits_on_local_network=false.
	lt::ip_filter pc_filter;
	const std::uint32_t global_class =
		1u << static_cast<std::uint32_t>(lt::session::global_peer_class_id);
	pc_filter.add_rule(lt::make_address("0.0.0.0"), lt::make_address("255.255.255.255"), global_class);
	pc_filter.add_rule(lt::make_address("::"),
		lt::make_address("ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff"), global_class);
	m_session.set_peer_class_filter(pc_filter);
}

int app::run()
{
	if (!m_service.start(m_config.listen_address)) {
		return 1;
	}
	m_log.start();
	spdlog::info("Server listening on {}", m_config.listen_address);
	m_daemon.run();
	spdlog::info("shutdown in main");
	m_service.stop();
	return 0;
}

}  // namespace ezio
