#include "app.hpp"

#include "spdlog/spdlog.h"

#include <libtorrent/libtorrent.hpp>

#include "raw_disk_io.hpp"

namespace ezio
{

// static
lt::session_params app::make_session_params(const config &cfg)
{
	lt::settings_pack p;
	// setup alert mask
	p.set_int(lt::settings_pack::alert_mask,
		lt::alert_category::error | lt::alert_category::status);

	// disable all encrypt to avoid bug https://github.com/arvidn/libtorrent/issues/6735#issuecomment-1036675263
	p.set_int(lt::settings_pack::out_enc_policy, lt::settings_pack::pe_disabled);
	p.set_int(lt::settings_pack::in_enc_policy, lt::settings_pack::pe_disabled);

	// disable uTP for better performance
	p.set_bool(lt::settings_pack::enable_outgoing_utp, false);
	p.set_bool(lt::settings_pack::enable_incoming_utp, false);
	p.set_int(lt::settings_pack::mixed_mode_algorithm, lt::settings_pack::prefer_tcp);

	// thread pool size from config (used for both I/O and hashing)
	p.set_int(lt::settings_pack::aio_threads, cfg.aio_threads);
	p.set_int(lt::settings_pack::hashing_threads, cfg.aio_threads);
	spdlog::info("Thread pool: aio_threads={} (used for both I/O and hashing)", cfg.aio_threads);

	// network buffer sizes
	p.set_int(lt::settings_pack::suggest_mode, lt::settings_pack::suggest_read_cache);
	p.set_int(lt::settings_pack::max_queued_disk_bytes, 128 * 1024 * 1024);
	p.set_int(lt::settings_pack::send_not_sent_low_watermark, 524288);
	p.set_int(lt::settings_pack::send_buffer_watermark, 128 * 1024 * 1024);
	p.set_int(lt::settings_pack::send_buffer_low_watermark, 32 * 1024 * 1024);

	// unified_cache size from config (default 512MB)
	// Note: cache_size is deprecated but still used by raw_disk_io
	// cache_size unit: number of 16KiB blocks
	// Convert MB to number of 16KB blocks: (MB * 1024) / 16
	int cache_blocks = (cfg.cache_size_mb * 1024) / 16;
	p.set_int(lt::settings_pack::cache_size, cache_blocks);
	spdlog::info("Cache size: {} MB ({} blocks)", cfg.cache_size_mb, cache_blocks);

	lt::session_params ses_params(p);
	if (!cfg.file_flag) {
		ses_params.disk_io_constructor = raw_disk_io_constructor;
	}
	return ses_params;
}

app::app(const config &cfg) : m_config(cfg), m_session(make_session_params(cfg)), m_daemon(m_session, m_config.slow_start, m_config.slow_start_period), m_service(m_daemon), m_log(m_daemon, m_daemon.get_io_context())
{
}

int app::run()
{
	m_service.start(m_config.listen_address);
	m_log.start();
	spdlog::info("Server listening on {}", m_config.listen_address);
	m_daemon.run();
	spdlog::info("shutdown in main");
	m_service.stop();
	return 0;
}

}  // namespace ezio
