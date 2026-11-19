# Security Policy

corvus runs inside other people's processes — robots, game servers, edge devices — and hands
model output to tools. Security reports are taken seriously and handled privately.

## Reporting a vulnerability

**Please do not open a public issue for a security problem.**

Report it privately through GitHub:
[**Report a vulnerability**](https://github.com/Ambar-Gupta22/corvus/security/advisories/new)
(repo → *Security* tab → *Report a vulnerability*).

Please include:
- the affected file/function and commit (or version);
- what an attacker controls (model output, a tool's result, a server response, config) and what
  they gain;
- a minimal reproduction if you have one.

What to expect — corvus is maintained in spare time, so these are targets, not guarantees:
- acknowledgement within **7 days**;
- an assessment and a fix plan within **30 days**;
- coordinated disclosure: we agree on a date with you, publish a GitHub Security Advisory with
  credit (unless you prefer otherwise), and request a CVE where warranted.

## Supported versions

corvus is pre-1.0. Only the latest commit on `main` receives security fixes. Once releases are
tagged, this section will list which ones are supported.

## Scope

In scope — anything in this repository, in particular:
- the HTTP transport (`src/httplib_transport.cpp`): URL/header validation, response size caps,
  TLS configuration, redirects;
- the agent loop and tool execution (`src/agent.cpp`, `include/corvus/tool.h`): anything that lets
  model output escape the limits a tool or the loop is supposed to enforce;
- argument parsing (`src/args.cpp`) and any crash or unbounded resource use reachable from model
  output, tool results, or server responses;
- the build: dependency pinning and CI.

Out of scope:
- vulnerabilities in a dependency that corvus does not reach (report those upstream — but do tell
  us if corvus is affected);
- prompt injection that only makes the model *say* something wrong, with no effect beyond what
  the application's own tools allow. Tools that let a model act on the world are the
  application's trust decision; reports showing corvus *weakens* a tool's guard are in scope;
- issues requiring an attacker who already controls the host process or its configuration.

## Dependencies

Third-party code is fetched at build time and pinned by SHA-256 archive hash in
`CMakeLists.txt` / `tests/CMakeLists.txt` (cpp-httplib, nlohmann/json, doctest). These are
monitored against the projects' GitHub security advisories and bumped by hand; GitHub Actions are
pinned by commit SHA and kept current by Dependabot.
