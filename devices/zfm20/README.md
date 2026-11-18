<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->
# zfm20

ZhianTec's ZFM-20 optical fingerprint module on a UART, as the model
`zfm20`: the packet protocol and its instructions, over a finger the host
puts on the sensor (ZFM-20 user manual V1.5, `ZFM_user_manual_V15.pdf`, as
Adafruit hosts it; VfyPwd 13h and ReadIndexTable 1Fh, which V1.5 lacks,
from Grow's R503 manual V1.4.1, the same protocol). Zephyr drives it as
`zhiantec,zfm-x0`.

A finger is a number. An image, a character file and a template hold the
number of the finger they came from, so two match when their numbers do.

## Bindings

| index | name | kind | required | resolved into |
| --- | --- | --- | --- | --- |
| 0 | `uart` | `uart` | yes | the module's TXD and RXD, `(tx, rx)` from the harness: 57600 8N1 from the factory |

```python
import metalclaw
fp = metalclaw.attach("zfm20", "fingerprint", uart=(9, 8))
fp.finger = 1        # finger 1 on the sensor; 0 lifts it
fp.score = 157       # what Match and Search report
fp.templates         # templates in the library
```

## Properties

| name | type | access | meaning |
| --- | --- | --- | --- |
| `finger` | u32 | rw | the finger on the sensor, 0 for none (at attach) |
| `score` | u32 | rw | the matching score Match and Search report, 100 at attach |
| `templates` | u32 | ro | templates in the library |
| `commands` | u32 | ro | command packages answered since attach |

## Protocol

Packages (5.1): EF01h, the 4-byte address (FFFFFFFFh), the identifier, a
2-byte length, the content and the 2-byte sum of identifier, length and
content. The module answers command packages addressed to it with an
acknowledge package; a bad checksum gets confirmation code 01h, and other
packages no answer.

| code | instruction | here |
| --- | --- | --- |
| 01h | GenImg | the finger into the image buffer; 02h with none on the sensor |
| 02h | Img2Tz | the image into CharBuffer1 or 2 (any other BufferID is 2); 15h with no image |
| 03h | Match | 00h and the score when both buffers hold one finger, else 08h |
| 04h | Search | the first page in the range holding the buffer's finger, with the score; else 09h |
| 05h | RegModel | 00h when both buffers hold one finger, else 0Ah |
| 06h | Store | the buffer into the page; 0Bh past the library |
| 07h | LoadChar | the page into the buffer; 0Ch for an empty page, 0Bh past the library |
| 0Ch | DeletChar | N pages from the page; 10h past the library |
| 0Dh | Empty | the whole library |
| 0Eh | SetSysPara | baud (4, N of 9600, answered at the old rate), security (5), package size (6); 1Ah for another |
| 0Fh | ReadSysPara | status, 0009h, 120, security, address, size code, baud N |
| 13h | VfyPwd | 00h for the password (0), else 13h |
| 14h | GetRandomCode | a 32-bit number |
| 17h | handshake | 00h |
| 1Dh | TempleteNum | the template count |
| 1Fh | ReadIndexTable | the page's 32-byte table, bit n of byte 0 template n |

The library holds 120 templates (1.2, the ZFM-20). `reset()` is a power
cycle: the buffers clear, and the library and settings stay, at the saved
baud; `reset(True)` restores the factory library and settings.

## Not modelled

The image and template transfers (UpImage, DownImage, UpChar, DownChar,
which answer 01h), SetAdder, the notepad, the time a capture takes, and
image quality.
