# Nextendo BCAT

BCAT is the Switch service that keeps a per-game delivery cache: battle schedules, news,
events and data tables that change without a game update. With online play through Nextendo
on (Configure > Network > "Play online through Nextendo Network"), Lichii downloads each game's
cache from nextendo.network before the game boots. The game reads it through the `bcat:*`
services. With Nextendo off, nothing is downloaded and the services read whatever the cache folder
already holds, as before.

The protocol is Nextendo's, from [wiki.nextendo.network/bcat](https://wiki.nextendo.network/bcat).
The download and install follow citron-nextendo's `src/citron/nextendo_package_bcat.h`, and the
service fixes follow citron-nextendo's `src/core/hle/service/bcat/`.

## The route

```
GET https://nextendo.network/api/bcat/<title id, 16 hex digits>
User-Agent: Lichii/<version>
X-Nextendo-Client-Id: <nextendo_client_id>
```

The route is public. Lichii sends no account token, and a sign-in changes nothing here. The
request goes from the host directly over HTTPS with certificate checks. It doesn't go through the
emulated network or its DNS redirection, and it counts against the Nextendo request budget.
The whole check, a Scarlet fallback included, has 15 seconds.

| Status | Meaning | What Lichii does |
| --- | --- | --- |
| 200 | `application/zip`: the title's whole delivery cache | Installs it when its SHA-256 differs from the installed one, or the cache has no directories |
| 204 | Nothing is published for the title | Keeps the local cache |
| other, or unreachable | | Keeps the local cache and boots anyway |

Pokémon Scarlet (`0100A3D008C5C000`) shares Violet's event: when Scarlet gets 204 or 404, Lichii
asks for Violet's (`01008F6008C5E000`) and installs it into Scarlet's folder. System titles
(`0100000000000000` to `0100000000001FFF`) are never asked for.

## The package

A ZIP whose entries are `<directory>/<file>`, nothing at the root and nothing deeper. There is
no encryption or signature at this layer; files the game encrypts itself (Splatoon 3's
`.pack.zs.enc`) are stored unchanged. Lichii rejects the whole package, and keeps the old cache,
when any of these holds:

- a name is empty, over 31 characters, ends with `.`, or uses a character the bcat services
  refuse: directory names take `A-Z a-z 0-9 _ -`, file names `A-Z a-z 0-9 _ .`
- an entry is a symbolic link, is encrypted, or uses a compression other than store or deflate
- two paths differ only by case
- there are more than 512 entries, the download is over 32 MiB, or the files add up to more than
  32 MiB
- a size or CRC-32 doesn't match the data, or the ZIP is split or truncated
- the package is empty
- for Splatoon 3 (`0100C2500FC20000`), any of `summary.yml`, `base`, `bh`, `wa`, `wb` and
  `wc.pack.zs.enc` is missing from `ap-default`, `eu-default`, `jp-default` or `us-default`

## On disk

Each title has its own folder in the emulated NAND, the folder the `bcat:*` services read:

```
<nand>/system/save/bcat/<TITLE ID>/
  .nextendo_bcat_hash          SHA-256 of the installed package (not a directory, so games never see it)
  vsdata/VSSetting_0.byaml
  vsdata/VSSetting_1.byaml
  coopdata/CoopSetting.byaml
```

The delivery cache belongs to the title, not to a user profile, as on a Switch. The package is
the same for every account, so every profile on this NAND shares it.

An install writes the new folder as `<TITLE ID>.nextendo-stage`, renames the old one to
`<TITLE ID>.nextendo-backup`, then renames the stage into place and deletes the backup. A
directory or file the new package doesn't have disappears. An install cut off between the two
renames is undone the next time the game opens its cache, with Nextendo on or off, or at the next
check. If the cache folder can't be opened or created, the services serve an empty cache.

## The services

- `EnumerateDeliveryCacheDirectory` lists the title folder's subdirectories.
- `IDeliveryCacheDirectoryService` opens a directory and lists its files with sizes and MD5
  digests.
- `IDeliveryCacheFileService` reads files and returns their size and MD5 digest.
- `RequestSyncDeliveryCache` and `RequestSyncDeliveryCacheWithDirectoryName` do no network work.
  The progress event is signalled and reports Done (9) with result 0. Command 3,
  `CreateDeliveryCacheProgressService`, which older games such as Splatoon 2 1.0.0 call,
  answers the same way.

## Log lines

At `Service.BCAT:Info`, once per launch while Nextendo is on:

| Line | Meaning |
| --- | --- |
| `Nextendo BCAT: <title>: installed package <sha256 prefix>: vsdata (2), coopdata (1)` | A new or changed package was installed for this launch |
| `Nextendo BCAT: <title>: package <sha256 prefix> unchanged` | The installed cache is current |
| `Nextendo BCAT: <title>: nothing published, keeping the local cache` | 204 |
| `Nextendo BCAT: <title>: ..., keeping the local cache` (warning) | Unreachable, HTTP error or a rejected package |

Then, while the game runs, `CreateDeliveryCacheStorageService` with the game's process id and
`Service.BCAT:Debug` lines for the directory and file opens.
