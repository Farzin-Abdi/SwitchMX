# libarchive 3.7.2 readers (patched)

These files replace the objects of the same name in devkitPro's `switch-libarchive` 3.7.2. Search for
"SwitchMX" in them to find every change. The headers are unchanged from the v3.7.2 tag; `config.h` is minimal
and matches that package.

- `archive_read_support_format_7zip.c`: AES-256/SHA-256 decryption, including encrypted file names.
- `archive_read_support_format_rar5.c`: AES-256 decryption of file data and headers (`-hp`), password checks,
  tweaked checksums, and encrypted volumes, where one CBC stream runs through all parts. Checks stored files'
  checksums, handles empty files in solid archives, and can skip solid data when only listing.
- `archive_read_support_format_rar.c`: the same for RAR 3/4 with AES-128, plus solid archives and PPMd filters,
  which the stock reader rejects. The escape character now survives a table change, as the format requires.

Key derivation and AES are in `source/minicrypto.c`. The crypto was written from the format's behaviour, and
no RARLAB code is included. RAR 1.x/2.x compression and passwords aren't supported.

Licensed under the BSD-style terms in `COPYING`. If `switch-libarchive` changes version, re-vendor from
that tag.
