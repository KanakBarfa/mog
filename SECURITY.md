# Security policy

## Supported versions

| Version | Supported |
|---------|-----------|
| 0.1.x   | yes       |

## Reporting a vulnerability

Use GitHub's private security advisories
(Security -> Advisories -> New draft security advisory) rather than a public
issue. Include a reproducing input where possible; for parser bugs, the
smallest crashing ITCH/MoldUDP64 byte prefix is ideal.

## Scope notes

mog is an offline analysis engine: it opens local files, mmaps captures, and
runs deterministic simulations. It does not open network sockets or expose
services. The primary untrusted-input surfaces are:

- ITCH 5.0 / MoldUDP64 byte streams (parser framing and message decode)
- simrun script CSVs and events logs read back by tools

Determinism anchors are part of the contract: any fix that changes digests
must be flagged in the PR and justified.
