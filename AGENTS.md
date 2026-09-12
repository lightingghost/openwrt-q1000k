# OpenWrt Q1000K branch policy

- `main` tracks official `openwrt/openwrt` upstream and must contain no
  Q1000K-specific changes, community imports, or local adaptations.
- `q1000k-support` is reserved for the upstream Q1000K support PR. Change
  this branch only when the user explicitly requests a PR-branch change.
  Do not put community packages or development work on it.
- `q1000k-dev` is the default branch for development, PR/community imports,
  packages, experiments, and Q1000K adaptations. Work here unless the user
  explicitly requests another branch.
- `q1000k-xgspon` is the user-requested branch for all Q1000K XGS-PON work,
  based on `q1000k-dev`. Keep XGS-PON plans, imports, drivers, userspace and
  board adaptations on this branch until the user requests integration.
  Keep the normal firmware builder on `q1000k-dev`; use a separate explicit
  branch/revision selection for experimental XGS-PON builds.
- Preserve imported commits as separate commits when possible, with original
  authorship and source commit references. Keep Q1000K-specific adaptations
  in follow-up commits; document partial imports instead of squashing them.
- Preserve existing uncommitted work when switching branches. Do not rewrite
  or force-push remote branches without explicit user authorization.
