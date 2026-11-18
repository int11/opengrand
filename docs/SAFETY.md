# Safety, risk and warranty

**Please read all of this before building or flashing anything.**

## Risk of damage

OpenGrand modifies the system firmware of your instrument. Doing so **may permanently damage ("brick") your
instrument**, corrupt per-device calibration data that cannot be regenerated, or cause malfunctions that
only show up later. The project includes several safety nets (signature checks, A/B slots with automatic
fallback, escape keys, recovery through Kawai's official update - see [FLASHING.md](FLASHING.md)), but none of
them is guaranteed to work on your instrument, in your situation, every time.

Known ways to make things worse:

- cutting power, or removing the USB stick, during a USB update;
- putting more than one firmware file, or wrongly named files, on the update stick;
- using a system file other than Kawai's V2.19 `CAx9_P06.SYS`, or an image built by someone else;
- flashing a model that is not listed as supported (CA701/CA901 use a different system firmware);
- uploading code over Bluetooth that was not built and tested from this repository.

## Warranty

Modifying the firmware will likely **void your manufacturer's warranty**, and Kawai and its dealers are under
no obligation to support or repair an instrument that has run modified firmware. OpenGrand is not affiliated
with, endorsed by, or sponsored by Kawai Musical Instruments Mfg. Co., Ltd.

## No warranty from this project

THIS SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT
LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO
EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, INCLUDING
DAMAGE TO YOUR INSTRUMENT OR LOSS OF DATA, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE
OR OTHER DEALINGS IN THE SOFTWARE. See also sections 15 and 16 of the [GNU GPL v3](../LICENSE).

**You use OpenGrand entirely at your own risk.**

## Your responsibilities

- Obtain Kawai's system update yourself, from Kawai, and use it in accordance with the terms under which you
  obtained it. This repository contains no Kawai firmware, code, keys or assets.
- **Do not distribute** images built from Kawai's file (`*.SYS`, `*.c79`), and do not ask others to send you
  theirs.
- Keep your keys (`result/stockfw/secrets/ota_key.bin`, the panel UI key) private; anyone with your
  Bluetooth update key and within radio range can install code on your piano.
- Check the laws that apply to you. Modifying software on a device you own for interoperability is treated
  differently in different countries.

## Reporting a safety problem

If you find a bug that could damage an instrument or lose data, please open a GitHub issue marked
**[safety]** with as much detail as you can (model, system version, OpenGrand commit, what you did, what the
screen showed). Do not include serial numbers, Bluetooth addresses or other personal data.
