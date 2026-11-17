# tests/github

The issue moderation bot (`tools/github/`).

| File | What it checks |
|---|---|
| `test_issues.py` | The issue moderation's reading of a report (`tools/github/classify.py`): the release from the form, RecompLauncher's report or the prose, the GPU without the CPU's vendor, empty reports, logs, known errors and their regressions, which maintainer replies close an issue and which player replies reopen it |
