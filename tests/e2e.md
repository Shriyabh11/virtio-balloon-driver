# Phase 2 E2E Result (MVP)

## Scope
Host daemon + shared memory + guest agent command/ack flow.

## Run Result
1. Started `host/balloond` with target `2147483648`.
2. Started `guest/shm_agent`.
3. Observed ack and actual update:
   - `cmd_seq=2`
   - `ack_seq=2`
   - `actual=2147483648`
4. Updated runtime target from daemon console:
   - `2147483648` -> `3221225472`
5. Observed reverse ack and actual update:
   - `cmd_seq=5`
   - `ack_seq=5`
   - `actual=3221225472`

## Conclusion
Shared-memory control loop is working:
- host writes command
- guest acknowledges
- bridge-reported `actual_bytes` follows target in both directions

## Note
This is Phase 2 MVP bridge behavior using `guest/shm_agent`.
Next step is wiring the same control path into guest kernel balloon driver flow.

