# Credits and third-party notices

`skqe` is licensed under the GNU General Public License v3.0 ([`LICENSE`](LICENSE)). It includes or
derives from the work below, whose authors keep their copyright and whose terms still apply to it.

## xEdit (TES5Edit / SSEEdit)

By ElminsterAU, with updates since 2012 by Hlp, Zilav and Sharlikran.
Source: <https://github.com/TES5Edit/TES5Edit>. Licence: Mozilla Public License 2.0, a copy of which is
in [`LICENSES/MPL-2.0.txt`](LICENSES/MPL-2.0.txt).

No xEdit source file is included, and `skqe` isn't a port of it. These parts of `skqe` come from xEdit's
definitions (`Core/wbDefinitionsTES5.pas`, `Core/wbDefinitionsCommon.pas`) and remain available under
the MPL 2.0, from the source linked above:

| Where | What comes from xEdit |
|---|---|
| `src/itm_ctda.inc` | Condition function indexes, names and parameter types |
| `src/unused_subrecords.inc` | The subrecords xEdit labels unused |
| `src/itm.cpp` | The rules for comparing records: ignored fields, sorted arrays, ignored flags, float sort keys and the layouts that follow xEdit's |
| `data/patterns.json`, `_display` and `_names` | xEdit's names for subrecords and tuple items |

`skqe`'s catalog was also checked value for value against a dump of xEdit's output, and its Quick Auto
Clean is the model for `skqe`'s identical-to-master check.

## puff (the inflate in `src/inflate.cpp`)

`src/inflate.cpp` is altered from `contrib/puff/puff.c` in the zlib distribution: restructured as C++,
with bounds checks, an output limit and the zlib wrapper added. It is not the original puff. The same
decoder is in bsa-reader, which is part of `skqe.exe`.

```
Copyright (C) 2002-2013 Mark Adler, all rights reserved
version 2.3, 21 Jan 2013

This software is provided 'as-is', without any express or implied
warranty.  In no event will the author be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required.
2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.

Mark Adler    madler@alumni.caltech.edu
```

## zlib

Used for compression by `skqeedit`, which is built from source and isn't in the release zip. The build
downloads zlib 1.3.1 from <https://github.com/madler/zlib>; its source isn't kept in this repository.

```
(C) 1995-2022 Jean-loup Gailly and Mark Adler

This software is provided 'as-is', without any express or implied
warranty.  In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required.
2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.

Jean-loup Gailly        Mark Adler
jloup@gzip.org          madler@alumni.caltech.edu
```

## SQLite

`third_party/sqlite` is the SQLite 3.54.1 amalgamation, by D. Richard Hipp and the SQLite developers
(<https://sqlite.org>). It is in the public domain.

## Companion libraries

Both are by the author of `skqe`, under GPL-3.0, and are compiled into `skqe.exe`:

- [bsa-reader](https://github.com/cecell/bsa-reader): reads BSA archives.
- [beth-asset-metadata](https://github.com/cecell/beth-asset-metadata): reads asset file headers.

## Trademarks

The Elder Scrolls, Skyrim and Creation Kit are trademarks of ZeniMax Media Inc. or its affiliates. This
project isn't affiliated with or endorsed by Bethesda Softworks or ZeniMax Media, and it includes no
game files.
