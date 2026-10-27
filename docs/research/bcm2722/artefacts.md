# BCM2722 RE: artefacts ledger

Where every Apple binary used in the BCM2722 video research comes from, and how to rebuild it bit for bit. **None of these files are in git.** They live in `~/Git/re/bcm2722/` on the work Mac. Only names, offsets and hashes are recorded here (issue #60).

## Sources

All the sources are Apple's public iPod Video (5.5G, "iPod_25") updater bundles. The URLs are the ones listed on the iPodWiki/TheAppleWiki firmware tables. No device was read.

| Version | File | SHA-1 (ipsw) | URL |
|---|---|---|---|
| 1.2.1 | `iPod_25.1.2.1.ipsw` | `47c59c9781f5fc5bf8c393d53f86ace6634c6c43` | `http://appldnld.apple.com.edgesuite.net/content.info.apple.com/iPod/SBML/osx/bundles/061-2790.20061206.iPr9t/iPod_25.1.2.1.ipsw` |
| 1.2.3 | `iPod_25.1.2.3.ipsw` | `6ef8101c4740b61da2b27cf46f14729e960a340d` | `…/061-4095.20071126.12bvn/iPod_25.1.2.3.ipsw` |
| **1.3** | `iPod_25.1.3.ipsw` | `0f2dd984c5d56475bdf3c14b4bc7541b799f2c85` | `…/061-2967.20080313.Cnvkg/iPod_25.1.3.ipsw` |

1.3 is the 5.5G's final firmware and is the main target. 1.2.x is kept for diffing.

## Extraction

```sh
unzip iPod_25.1.3.ipsw -d 1.3                      # -> Firmware-25.6.3, manifest.plist
tools/bcmre/fwextract.py 1.3/Firmware-25.6.3 1.3/img
MTOOLS_SKIP_CHECK=1 mcopy -s -n -i 1.3/img/rsrc.fat@@1536 ::/Resources/VideoCore 1.3/vc/
```

The partition directory starts at `0x4200` and holds 40-byte `!ATA` entries. Each image starts with a 2 KB header, then the payload:

| Image | devOffset (1.3) | Length | Payload |
|---|---|---|---|
| `osos` | `0x4800` | `0x736000` | ARM OS, loaded at `0x10000000`. The vectors sit at payload offset 0, with the `portalplayer` tag at `+0x20` |
| `rsrc` | `0x73b000` | `0x500800` | FAT16 volume "iPodResources". The boot sector is at payload `+0x600` |
| `aupd` | `0xc3c000` | `0x106800` | Flash updater (not yet examined) |

## Payload hashes (SHA-256)

| File | 1.2.1 | 1.2.3 | 1.3 |
|---|---|---|---|
| `osos.code` | `84549ab735ff8345361d42ae517643ef5c6a1de736a52eaf201f128cc32e6836` | `17f61513172e035669632f9edb7e57e93e3fe90b6a70dfcb772ef3cfa3d54051` | `57eb6e14b5c2f809b0b93bb56782c0b520551ed30de978bc62189d30255db48c` |
| `rsrc.fat` | `5b7a19651c4bc8419e9d48d0886e83485a540b724735a794d76c939ae60b1cfe` | `50fce06671b921b9ccdc36bd5816720b680c782eb33bf4a808b9963709ad8781` | `1ca691f43aa4a386b837259ae59a3858bd13be31252f30f8c1268d8c6b5a1134` |

### `/Resources/VideoCore` (1.3)

| File | Size | SHA-256 | Same in 1.2.x? |
|---|---|---|---|
| `Boot/vmcs.bin` | 201376 | `cde4b5a9c9a020b6e23de1be0492d9a853b7d974c4060462d5f57a80e47f55be` | no (1.2.1 `75584dcb…`, 1.2.3 `d7eec71c…`) |
| `Boot/RenderServer.bin` | 104540 | `a75c4c2060e6e9d4e85e31ac7ab6f15ce123805e06874c09fc3de3aea2e3e001` | yes |
| `Library/h264dec.vll` | 106960 | `3915ec9295dc0d9cf9405810a66788e5b544c819347e05a350df6857120a9e8f` | no (1.2.x `32559278…`) |
| `Library/mpg4dec.vll` | 147232 | `08ba2f33f267803746c3b3de025e615f4a4b899611d7a4e3eea819f39d3697fe` | yes |
| `Library/aacdec.vll` | 52664 | `0a8c63080120a8af36a2d68ed4ade7eae81eee15f1e344f66cd49a508162c2cc` | yes |
| `Library/mplayer.vll` | 51620 | `c100e0cb0782277f81d2b9bceb29340df70d2993e34f77248a8e3c1979bc1fc6` | yes |
| `Library/passthruhandler.vll` | 6528 | `0b657b1a4b50198da3ad3d55ecf9d239ffbae8d7da7b3aaca138dcdae5b6d019` | yes |
| `Library/slideshow.vll` | 47940 | `3d620d9c6b9bc6884a4cc597e5fad254c09d4f7e5c6275cba4faf2adceea303e` | yes |

The rsrc volume also carries Apple's fonts, including PodiumSans. They are irrelevant here and must never be shipped (CLAUDE.md rule 2).

## Licensing stance
We use these files only to learn the host↔VideoCore protocol, for interoperability. A future ProkBox video player would load the VideoCore files **from the owner's own iPod at runtime**, the same way Rockbox already uploads the NOR-flash `vmcs` (`lcd-video.c`). How such files get onto a unit is an open owner question; see `go-no-go.md` once it exists. We never ship them.
