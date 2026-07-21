# Licensing

This repository contains source code under compatible licenses. License
notices on individual files take precedence over this summary.

## Xbox 360 distribution

The combined Xbox 360 program and its source distribution are provided under
the **GNU General Public License version 3 only (GPL-3.0-only)**. The complete
license is in [LICENSE](LICENSE).

The files in `src/xbox360-xdk/` are GPL-3.0-only. They derive in substantial
part from the Xbox 360 port in
`ceilingtilefan/Butterscotch-360`, commit
`7f8f1ea6044dbc55560dfbd2ca9a2f45e472c02e`, originally contributed by
Maxine, with later reference work from `flaf1x/Butterscotch360-Refresh` and
later modifications by Ral-sei.

## Upstream MPL files

Files inherited from the upstream Butterscotch project remain licensed under
the **Mozilla Public License 2.0 (MPL-2.0)**. A copy is retained at
[LICENSES/MPL-2.0.txt](LICENSES/MPL-2.0.txt). This repository does not claim
to relicense those individual files.

Under section 3.3 of MPL 2.0, MPL-covered files may be included in a Larger
Work distributed under GPL-3.0-only, provided the MPL requirements continue
to be met for the MPL-covered files. The upstream MPL license contains no
Exhibit B notice declaring incompatibility with secondary licenses.

Third-party files under `vendor/` retain their own notices and licenses.

## Proprietary components and game data

The Microsoft Xbox 360 XDK, its headers, libraries, tools, and runtime
components are not included in this source repository. They remain governed
by Microsoft's applicable agreements. GPL licensing of this project's source
does not grant permission to obtain or redistribute the XDK.

Game data (including `data.win`), compiled XEX files, SDK libraries, and
other proprietary assets are not part of the source distribution and are not
covered by this project's GPL grant.
