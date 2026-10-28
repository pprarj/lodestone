# Magelight UI - API header (vendored)

The public C++ API header of Magelight UI, vendored so this repository builds
without the Magelight UI mod installed. Nothing here is ours: do not edit these
files. To move to a newer Magelight UI, replace the whole folder and regenerate
the table below.

## License - MIT, and that is why these are tracked

`LICENSE-MIT` sits alongside. It is the `LICENSE` of the Magelight UI
repository at the tag below, which is MIT, copyright 2026 Severause, and the
header (`api/MagelightUI_API.h`) is the file that license is for: it is the
header "other SKSE plugins compile against". MIT adds no restriction, so the
header can live inside this GPL-3.0-only repository.

The MIT grant covers the header code only. It does not relicense Magelight UI's
implementation, which reaches a player under the terms in that package's
`NOTICES.txt`, nor Ultralight, nor CommonLibSSE-NG, nor anything a consumer
links.

NOT VENDORED: `api/MagelightUI_Mod.h`, the convenience wrapper in the same
folder. This project uses the C function-pointer struct directly and does not
need it, and nothing was checked about the wrapper's license.

## Magelight UI is not a dependency

Neither Magelight UI nor Prisma UI nor Meridian UI is required. They are
detected at runtime; with none installed the WebUI bridge answers sentinels and
logs nothing but one info line. This header is a compile-time interface, not a
runtime requirement. `MAGELIGHT_API_NO_LOADER` is NOT defined: the backend uses
`RequestApi4()` to resolve `Magelight_RequestApi` from an already-loaded
`Magelight.dll`.

## Version

    tag      v1.6.0
    commit   984373995555fd95925556b05d9baa221f0746b8
    origin   github.com/Severause/MagelightUI, file api/MagelightUI_API.h
    API      MagelightApi4 (kApiVersion4); 1.6.0 appended SetViewGamepad and
             GetViewGamepadState at the end of that struct, nothing before them
             changed. The backend gates on hostVersionNumber >= 10500 (1.5.0).

The installed Magelight UI on the development instance is 1.6.0 (its
`VERSION.txt`), which is what this tag was matched against.

## Origin, and line endings

Copied with `git show v1.6.0:<path>` from a clone of the repository, so the
bytes are the repository's own: **LF** (`git ls-files --eol` says `i/lf`). A
Windows checkout with `core.autocrlf` turns them into CRLF, and the `sha256`
below is over the LF bytes, which is what is in this folder.

## Files, as vendored

`sha256` is over the file as it sits here, full digest.

| File | Bytes | sha256 |
|---|---|---|
| `MagelightUI_API.h` | 64093 | `7e74aca26bb1c5a52e0250409cfcbea44e9c55c85a1a261387ba952973c4d80d` |
| `LICENSE-MIT` | 1066 | `1d3bc073f6ccb5f710f2cc0990ab81ab70832d4a43b02f6703042fa355f07203` |

Short form, 16 hex: `MagelightUI_API.h` `7e74aca26bb1c5a5`, `LICENSE-MIT`
`1d3bc073f6ccb5f7`.
