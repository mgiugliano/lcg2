# inih (vendored)

INI file parser by Ben Hoyt, <https://github.com/benhoyt/inih>, tag **r58**,
files fetched unmodified from `raw.githubusercontent.com/benhoyt/inih/r58/`
on 2026-11-18. Licence: New BSD, in `LICENSE.txt` (kept as it came; the MIT
notice of LCG2 does not apply to these files).

| file | SHA-256 |
|:--|:--|
| `ini.c` | `217fa78f0380669c91a565f3e414d0a8be0f7a7fdfd1e3839f213e86aa574a95` |
| `ini.h` | `b5342061a317f8cedd1103c10e241f74c16b5944d62deb9ab9e3e8a3f1ca4e5d` |
| `LICENSE.txt` | `9ae3b39e83e9158e44b67733baa3bb2d84e80efdcfb14a5820210a42c7abdf7f` |

Not modified: it is configured with macros from the Makefile (`INIFLAGS`), the
same ones for `ini.c` and for `src/config.c`:

- `INI_HANDLER_LINENO=1`: the handler gets the line number, for error messages.
- `INI_CALL_HANDLER_ON_NEW_SECTION=1`: the handler sees empty sections.
- `INI_ALLOW_MULTILINE=0`: an indented line is an error, not a continuation.
- `INI_STOP_ON_FIRST_ERROR=1`: report the first mistake.
- `INI_MAX_LINE=1000`: longer lines are refused (checked in `config_load`).

Known r58 limit: section names are cut at 49 characters (a fixed constant, not
an option); `config.c` refuses any name of 47 characters or more, so a cut name
is never accepted.
