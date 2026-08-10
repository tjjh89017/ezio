# Security Policy

## Supported Versions

EZIO is developed on `master` and shipped as tagged releases. Security fixes go
into the latest release line only; older tags are not backported. If you are
running anything other than the most recent release, upgrade first and check
whether the issue still reproduces.

If you use EZIO through Clonezilla Lite Server, also report the issue to the
[Clonezilla project](https://clonezilla.org/) so the bundled copy gets updated.

## Reporting a Vulnerability

**Please do not open a public GitHub issue for a security vulnerability.**

Report it privately through GitHub's coordinated disclosure flow:

1. Go to <https://github.com/tjjh89017/ezio/security/advisories/new>.
2. Describe the issue, the affected version or commit, and the impact.
3. Include reproduction steps: the command line used, the torrent/partition
   layout, and any relevant `ezio` log output (run with a raised log level).

If GitHub private reporting is unavailable to you, email the maintainer
directly at <tjjh89017@hotmail.com> instead.

### What to expect

- **Acknowledgement:** within 7 days.
- **Assessment:** a severity judgement and a fix plan within 30 days.
- **Disclosure:** the advisory is published once a fixed release is tagged.
  Reporters are credited unless they ask not to be.

This is a volunteer-maintained project, so timelines are best effort.

## Scope

EZIO writes directly to raw block devices and moves disk images over the
BitTorrent protocol, so the following are in scope:

- Memory-safety issues in the disk I/O, cache, or torrent handling paths
  (`raw_disk_io`, `unified_cache`, `buffer_pool`, `partition_storage`).
- Anything that lets a remote peer cause a write outside the offsets declared
  by the torrent, or otherwise corrupt the target partition.
- Authentication or authorisation flaws in the gRPC control service.

The following are **out of scope** — they are documented design properties of
a LAN deployment tool, not defects:

- The gRPC control service and the BitTorrent tracker are unauthenticated and
  unencrypted by design. EZIO is intended for a trusted deployment LAN; do not
  expose these ports to an untrusted network.
- Running `ezio` as root and destroying data on the target partition. That is
  the tool's purpose.

## Security Practices

This repository runs [CodeQL](https://codeql.github.com/) static analysis on
every push and pull request, and publishes an
[OpenSSF Scorecard](https://scorecard.dev/viewer/?uri=github.com/tjjh89017/ezio)
report weekly.
