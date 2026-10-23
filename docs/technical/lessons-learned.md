# Integration checks

Keep the shared toolkit's mechanisms separate from a port's compatibility
policy. Captured addresses, asset formats, priorities and title-specific
fallbacks belong to the consuming project.

- Load memory and executable ranges from the input image. Validate guest
  spans and retain mapping lifetimes; copying bytes creates a snapshot,
  not a writable alias.
- Preserve guest register, stack and thread contracts when calling generated
  functions. Diagnose unresolved indirect calls at their source; returning a
  plausible value does not establish correct behavior.
- Route only supported host faults to device handlers. Guest exceptions need
  their own dispatch semantics; skipping a faulting instruction can corrupt
  execution. See [SEH handling](seh-handling.md).
- Separate device mechanisms from application recovery through explicit
  configuration or hooks. Defaults must not depend on a captured title layout.
- Bound diagnostics and preserve the failure context: input revision, guest
  address, caller, relevant registers and native symbols. See
  [pipeline debugging](../pipeline/06-debugging.md).
- Prove one contract at a time with synthetic checks, then validate the port's
  actual path and assets. Menu/loading output and draw counts do not prove
  driving, sound or rendering fidelity. See [support and validation](gap-analysis.md).

These are review criteria, not claims that every runtime subsystem already
satisfies them. Keep outstanding gaps explicit.
