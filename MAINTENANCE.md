# Provenance

This convergence branch starts from XBribo/CS2-Bot-Hider
941e642b35f649b4cf08005c2049e57519cfaefa. It retains upstream native hooks,
Linux support, configuration, avatars, authorship and license.

The synchronous presentation bridge, lease service, lifecycle cleanup and
regression tests were adapted from unicbm/demotracer
e63d565508517b48706ab7c50b55abd0520f91ca, under server/runtime/BotHider.
That code is itself an AGPL-3.0-only maintained derivative of this project.
Contributors include XBribo and unicbm.

The neutral contract lives in BotHiderApi. DemoTracerBotHiderApi preserves the
existing DemoTracer contract and forwards to the neutral service.
BotController and replay execution remain outside this repository.

BotHider and these changes remain AGPL-3.0-only; see LICENSE.
This branch is an integration candidate, not an upstream-approved release.
