# Coding-Agent Instructions — Digital Andon System

This document defines how coding agents must work in this repository. It applies to firmware, backend, dashboard, contracts, deployment, tests, and documentation.

> Repository automation commonly discovers `AGENTS.md` with uppercase letters. If the chosen tooling requires automatic discovery, copy or rename this file to `AGENTS.md` once and update repository references. Do not maintain two divergent instruction files.

## 1. Mission

Build a reliable Digital Andon system for pipe-production operators using an ESP32 480×320 LVGL terminal, a Node.js/Fastify backend, MQTT, PostgreSQL, and a responsive supervisor dashboard.

The system must prioritize:

1. Correct incident state and auditability.
2. Honest offline behavior and loss-resistant request delivery.
3. Fast, simple operator interaction.
4. Safety boundaries.
5. Maintainable contracts and modular code.

## 2. Read order before changing code

1. Read this file completely.
2. Read [PRD.md](PRD.md) for product scope and acceptance criteria.
3. Read [design.md](design.md) before changing firmware screens or interaction behavior.
4. Read [architectur.md](architectur.md) before changing modules, data flow, MQTT topics, API contracts, persistence, security, or deployment.
5. Read relevant ADRs and nearby tests before editing an existing module.

If documents conflict, use this precedence:

1. Explicit current user instruction.
2. Safety and security constraints.
3. Accepted ADR.
4. PRD.
5. Architecture.
6. Design specification.
7. Existing implementation.

Do not silently choose between unresolved contradictions. Record the conflict and request a decision when it materially changes behavior.

## 3. Non-negotiable rules

- Never implement emergency stop, safety interlock, or hazardous machine control through this application.
- Never connect the ESP32 directly to PostgreSQL, ERP, MES, or CMMS databases.
- Never store shared production credentials in firmware source.
- Never report a locally queued request as backend accepted or responder notified.
- Never bypass the incident state machine with direct database updates.
- Never remove idempotency from device commands or backend mutations.
- Never allow responder resolution to imply operator confirmation automatically.
- Never log secrets, access tokens, passwords, full certificates, or unrestricted personal data.
- Never add a new dependency without documenting why existing tools are insufficient.
- Never change MQTT or HTTP payloads without updating versioned contracts and tests.

## 4. Scope discipline

### MVP scope

- Provisioned terminal and heartbeat.
- Four assistance categories and configurable reasons.
- Request, acknowledgement, handling, resolution, reopen, close, and cancellation.
- Persistent offline queue.
- Live dashboard and notification/escalation workflow.
- Audit trail and basic reports.

### Out of scope unless explicitly requested

- Full MES scheduling and inventory.
- Predictive maintenance and AI diagnosis.
- PLC safety or motion control.
- General chat, email client, or large on-device forms.
- Microservice decomposition for its own sake.
- Kubernetes for the initial pilot.

When a request expands the scope, identify the affected requirements, architecture, tests, migration, and operational cost before implementing it.

## 5. Working method

For every non-trivial change:

1. State the intended outcome and affected components.
2. Inspect existing code, contracts, tests, migrations, and working-tree changes.
3. Preserve unrelated user changes.
4. Make the smallest coherent change that satisfies the requirement.
5. Add or update tests in the same change.
6. Run relevant format, lint, type, unit, integration, and hardware checks.
7. Update documentation and contracts when behavior changes.
8. Report what changed, evidence of verification, and remaining risks.

Do not claim success if tests were skipped or hardware behavior was only inferred. State exactly what was and was not verified.

## 6. Repository ownership

| Path | Responsibility |
|---|---|
| `firmware/` | ESP32 drivers, LVGL, application state, persistence, MQTT |
| `backend/` | Domain rules, API, ingress, workers, database migrations |
| `dashboard/` | Responder and administrator web interface |
| `contracts/` | Versioned MQTT and HTTP schemas with examples |
| `deploy/` | Compose, proxy, broker, monitoring, operational configuration |
| `docs/adr/` | Architecture Decision Records |

Keep domain rules in the backend incident module. Do not duplicate transition rules in controllers, MQTT handlers, dashboard components, or SQL triggers.

## 7. Incident model rules

Valid states:

`OPEN`, `ACKNOWLEDGED`, `HANDLING`, `RESOLVED`, `REOPENED`, `CLOSED`, `CANCELLED`.

Valid normal transitions:

- `OPEN → ACKNOWLEDGED`
- `OPEN → CANCELLED`
- `ACKNOWLEDGED → HANDLING`
- `ACKNOWLEDGED → CANCELLED` only when policy permits
- `HANDLING → RESOLVED`
- `RESOLVED → CLOSED`
- `RESOLVED → REOPENED`
- `REOPENED → HANDLING`

Every transition must:

- Validate current state and optimistic version.
- Validate actor role and plant/line scope.
- Use an authoritative backend timestamp.
- Append an immutable incident event.
- Add required outbound messages to the transactional outbox.
- Return the updated state/version.

Invalid or stale transitions must return stable error codes; do not guess the intended state.

## 8. Contract rules

### MQTT

- Topic namespace begins with `andon/v1/`.
- Device events use QoS 1 and unique `eventId` plus `idempotencyKey`.
- Assume duplicate and out-of-order delivery.
- Validate every envelope and payload against a versioned schema.
- Do not include secrets or personal data in topic names.
- A schema-breaking change requires a new major topic/schema version.

### HTTP

- Base path is `/api/v1`.
- Validate request params, query, headers, and body.
- Use UTC RFC 3339 timestamps.
- Use cursor pagination for unbounded history.
- Use stable machine error codes and safe human messages.
- Mutations support idempotency where retries are plausible.

### Compatibility

- Additive optional fields are preferred.
- Firmware must ignore unknown optional fields safely.
- Backend must reject unknown schema versions explicitly.
- Update contract fixtures and consumer/provider tests with every contract change.

## 9. Firmware rules

### Platform

- Arduino framework under PlatformIO.
- LVGL 8.4 for the first stable implementation unless an ADR approves another version.
- Pin assignments and display/touch driver selection belong in board-specific configuration, not scattered constants.

### UI

- Only the LVGL/UI task may mutate LVGL objects.
- Use the fixed screen and component rules from `design.md`.
- Minimum effective target is 48×48 px; primary actions are 64 px or taller.
- Avoid a keyboard in normal operation.
- Use labels and icons in addition to status colors.
- Keep one dominant decision per screen.
- Avoid full-screen image assets, heavy gradients, frequent animations, and high-frequency redraws.
- Update timers once per second without reconstructing the full screen.

### State and transport

- Treat UI as a projection of application and incident state.
- Persist outbound command before attempting network delivery.
- Retain the same idempotency key across retries and reboot.
- Use bounded exponential backoff with jitter.
- Reconcile with backend after reconnect.
- Backend-accepted state overrides cached projections, but local pending commands remain visible until reconciled.

### Embedded reliability

- Avoid dynamic allocation in recurring hot paths where practical.
- Bound queues, strings, payloads, logs, and task stack sizes.
- Check all storage and serialization failures.
- Limit flash writes and batch diagnostic persistence.
- Watch free heap, maximum block, reconnect count, queue depth, loop/task health, and reset reason.
- Test power removal during queue/config writes.

## 10. Backend rules

### Language and style

- Node.js 20+ and TypeScript with strict mode.
- Fastify for HTTP and MQTT.js for broker integration.
- Keep handlers thin: validate, authorize, invoke application/domain service, map result.
- Use domain-specific names; avoid generic `utils`, `helpers`, or `manager` dumping grounds.
- Prefer explicit dependency injection through constructors/factories over hidden globals.

### Persistence

- PostgreSQL schema changes require migrations.
- Use parameterized SQL or a reviewed query builder/ORM.
- Incident update, event append, and outbox insert are one transaction.
- Use optimistic versioning and database constraints for idempotency.
- Do not use destructive migration shortcuts in production.
- Backfill separately from schema deployment for large tables.

### Async work

- Notifications and enterprise integrations execute after commit through outbox-backed workers.
- Workers must be idempotent and retry with limits/backoff.
- Dead-letter or terminal failure must remain observable and recoverable.
- External integration failure must not roll back an accepted incident.

## 11. Dashboard rules

- Optimize active incidents for fast scanning, not dense analytics.
- Show category, station, age, status, owner, and SLA state without opening detail.
- Require explicit action for acknowledge, start handling, resolve, cancel, and override.
- Use live updates but reconcile from REST after reconnect.
- Enforce permissions in backend; hiding a button is not authorization.
- Preserve filters in the URL where practical.
- Design responsive views for supervisor desktop and responder mobile use.
- Keep operator terminal wording consistent with HMI labels.

## 12. Security rules

- Unique device credentials and restrictive MQTT ACLs.
- TLS required outside isolated local development.
- Dashboard uses OIDC/SSO when available and RBAC with plant/line scope.
- Validate and bound all external data.
- Rate-limit device events, login, administration, and mutation endpoints.
- Redact secrets and minimize personal data.
- Use secure secret injection in deployment; do not commit `.env` secrets.
- Add authorization tests for every command and transition.
- Treat firmware artifacts and update channels as supply-chain assets.

## 13. Testing requirements

### Required for every state-machine change

- One test for the allowed transition.
- Tests for prohibited source states.
- Permission test.
- Stale-version/concurrency test.
- Audit-event assertion.
- Outbox-event assertion.

### Required for every MQTT command

- Valid fixture.
- Schema rejection fixture.
- Duplicate delivery test.
- Out-of-order or stale version test where relevant.
- Result correlation test.

### Required for firmware workflow changes

- State reducer/application logic test where host testing is practical.
- Touch flow review against design target sizes.
- Offline/retry behavior test.
- Restart recovery test for persisted state.
- Hardware verification on the target board before declaring production-ready.

### Required integration scenarios

1. Request to operator-confirmed close.
2. Offline queue and successful reconciliation.
3. Duplicate delivery.
4. SLA escalation.
5. Notification/provider failure.
6. Device/dashboard convergence after reconnect.

## 14. Performance budgets

### Device

- Immediate pressed-state feedback under 100 ms.
- Boot to cached UI target under 10 seconds.
- Timer update at 1 Hz.
- No unbounded queue, list, log, or payload.
- Record actual heap and frame behavior on target hardware.

### Backend

- Healthy-network request acceptance p95 under 2 seconds end to end.
- Dashboard update p95 under 5 seconds.
- Avoid N+1 queries in active board and reporting endpoints.
- Index query patterns after inspecting actual execution plans.
- Do not optimize by removing correctness, audit, authorization, or idempotency.

## 15. Observability requirements

- Structured logs with `correlationId` and relevant incident/device/station identifiers.
- Metrics for heartbeats, queue depth, duplicates, invalid messages, state age, outbox backlog, notification delivery, and API latency.
- Alerts must be actionable and identify plant/line scope.
- Do not log raw credentials, certificate bodies, or sensitive request headers.
- Firmware debug logs must be rate-limited and removable/reducible for production builds.

## 16. Documentation requirements

Update documentation in the same change when modifying:

- Product behavior or acceptance criteria.
- Screen flows, labels, colors, or touch interactions.
- Incident states or transition rules.
- MQTT topics or payload schema.
- HTTP endpoints or error codes.
- Database model or migrations.
- Deployment, provisioning, secrets, backup, or restore processes.
- Safety boundary or integration responsibility.

Significant architectural choices require an ADR containing context, decision, alternatives, consequences, and rollback/migration considerations.

## 17. Definition of done

A change is done only when:

- Requirement and acceptance behavior are clear.
- Code follows the responsibility boundaries.
- Contracts and migrations are versioned and reviewed.
- Relevant automated tests pass.
- Formatting, linting, and type checks pass.
- Security and authorization effects are evaluated.
- Offline, retry, duplicate, and failure behavior are considered.
- Documentation is updated.
- Hardware-dependent claims identify actual hardware evidence.
- Deployment and rollback implications are stated.
- No unrelated user work is overwritten.

## 18. Review checklist

- [ ] Does this change preserve the safety boundary?
- [ ] Is authoritative state still owned by the incident domain?
- [ ] Can duplicate delivery produce duplicate incidents or side effects?
- [ ] What happens during network, broker, database, or integration failure?
- [ ] Is the operator UI honest about accepted versus locally queued state?
- [ ] Are state transition and authorization tests present?
- [ ] Are touch targets and labels compliant with `design.md`?
- [ ] Are logs and metrics sufficient without exposing secrets?
- [ ] Are schema, contracts, migrations, and docs synchronized?
- [ ] Is the proposed complexity justified for the pilot?

## 19. Change handoff format

When completing implementation work, report:

1. **Outcome:** What behavior now works.
2. **Files:** Main files changed.
3. **Contracts/data:** API, MQTT, migration, or configuration effects.
4. **Verification:** Exact checks run and results.
5. **Hardware status:** Tested, simulated, or not tested.
6. **Risks:** Remaining limitations or follow-up decisions.

