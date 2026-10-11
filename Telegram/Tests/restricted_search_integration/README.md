# Production restricted-search integration

The executable includes `api/restricted_search_core.h`, the same production
coordinator instantiated by `api_restricted_global_search.cpp`. It does not
implement its own start/dispatch/response/cancel/retry/page state machine.
Only external facts, transport completion order, the clock and data decoding
are controlled by the fixtures. MergeEngine and AccountSchedule remain the
existing production implementations.

Run from the candidate parent checkout:

```powershell
./tools/test-restricted-search-integration.ps1
```

The default output is the dedicated `build-search-integration-tests` directory.
The script compiles using MSVC C++20 `/W4 /WX` and then runs the executable.
Assertions use throwing checks, so release/NDEBUG does not disable them.

Coverage includes Main/Archive/all, type filters, cached-only exclusion,
folded/explicit community rules, Unknown and full-info readiness, pinned
reload version/success/failure, preparation and sent-request deadlines,
generation cancellation and late success/failure, destruction, fresh retry,
raw cursors, equal-date boundaries, filtering/deduplication, buffered paging,
protocol errors, input delays, shared account concurrency/send interval,
FloodWait cooldown/bounded retries and recovery of 106 candidates. Synchronous
dispatch cancellation and cancellation during lazy decoding are also covered.

The Transport callback supplies a **synchronous lazy data decoder**. Core
checks its generation and request ticket before invoking it, and checks again
before accepting its output. Test decoders record invocation, proving that
late responses do not write the data model. The production adapter supplies
the real MTP decoder and Session ingestion implementation.

The client object compile and real Session/MTP adapter integration remain
separate validation. These deterministic cases do not establish real-account
UI behavior, persisted settings, or the original real-world performance
metrics, and do not automatically complete Comet acceptance.
