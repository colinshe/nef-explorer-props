# NEF Explorer properties

Windows Explorer property handler for Nikon `.nef` and `.nrw` files. After it is installed, File Explorer's details pane and the file **Properties → Details** page show the shooting information that is already inside the raw file.

Microsoft's [Raw Image Extension](https://apps.microsoft.com/detail/9nctdw2w1bh8) can show a thumbnail and the pixel size. It does not pass the camera EXIF block through to those details. This handler fills the gaps. It does not modify the photograph.

## What Explorer shows

| Detail | Source in the NEF |
|---|---|
| Camera maker, camera model | EXIF Make, Model |
| Lens maker, lens model | EXIF LensMake, LensModel |
| Exposure, f-number, ISO, focal length | standard EXIF |
| Date taken, including seconds | EXIF DateTimeOriginal, shown in this PC's local time |
| Latitude, longitude, altitude | EXIF GPS, in the same degree / minute / second form Windows uses for JPEG |
| Pixel size and bit depth | left to Windows' own photo handler when it already has them |

Date taken uses a dedicated field because Windows formats `System.Photo.DateTaken` with the short time pattern, which stops at the minute. The field added here keeps the seconds (`6:45:57 AM`, not `6:45 AM`).

GPS is shown only when the file actually contains it. A raw file with no location leaves latitude, longitude, and altitude blank.

## Why a separate handler

Explorer does not read EXIF itself. The Details page asks the registered property handler for the file extension. For `.nef` and `.nrw` that handler is `PhotoMetadataHandler.dll`, fed by the Raw Image Extension. On current Nikon bodies (tested with a Z6 III lossless NEF) it returns dimensions and leaves the camera and GPS groups empty.

Windows then treats latitude, longitude, and altitude as calculated values. Putting a single decimal coordinate into the store does not show up. Explorer builds those three lines from:

- degree, minute, and second numerators and denominators
- `N` / `S` and `E` / `W`
- altitude numerator, denominator, and above/below sea level

This handler supplies that layout, matching what Explorer already produces for a JPEG.

## Requirements

- Windows 10 or 11, 64-bit
- Administrator approval once, so Explorer can be pointed at the handler
- The free Raw Image Extension from the Microsoft Store, if you also want thumbnails. Camera, date, and GPS details do not need it.

The handler is read-only. It will not save ratings, titles, or GPS back into the NEF.

## Install on another PC

No compiler is required. Download the latest release zip from <https://github.com/colinshe/nef-explorer-props/releases>, or copy this folder from a PC where it is already built. The zip contains `NefPropHandler.dll`, `nef-datetaken.propdesc`, `register.ps1`, and `unregister.ps1`.

1. Unzip it to a permanent folder, for example `C:\Tools\nef-explorer-props`. Do not leave it in Downloads. Explorer loads the DLL from this path.
2. Open PowerShell in that folder and run:

```powershell
powershell -ExecutionPolicy Bypass -File .\register.ps1
```

3. Approve the administrator prompt. Explorer restarts, and open folder windows close.
4. Right-click a `.nef` file, choose **Properties**, then **Details**.

Run the same `register.ps1` again for each Windows account that should see the details. Removal is `unregister.ps1` in that same folder.

To install from source instead, clone the repository and follow [Build](#build), then run `register.ps1`.

## Build

From this folder:

```bat
build.bat
```

`build.bat` compiles `NefPropHandler.dll` and a small `test_handler.exe`. The DLL is linked so it does not need `libwinpthread-1.dll` beside it. Explorer would not find that DLL.

Check one file without registering anything:

```bat
test_handler.exe NefPropHandler.dll C:\photos\DSC_0001.NEF
```

`test_handler.exe lookup C:\photos\DSC_0001.NEF` reads through the registered handler instead of loading the DLL directly.

## Install

```powershell
powershell -ExecutionPolicy Bypass -File .\register.ps1
```

Approve the administrator prompt. The script:

1. Registers the COM class for the current user.
2. Points the system `.nef` and `.nrw` property handlers at this DLL. The previous handler id is saved under `HKCU\Software\PhotoGeoTagger\NefPropertyHandler`.
3. Registers `nef-datetaken.propdesc` so Date taken can include seconds.
4. Sets this user's Explorer detail lists for `.nef` and `.nrw`.
5. Restarts Explorer so the new handler loads. Open folder windows close.

The DLL path is the folder you registered. Do not move the folder without running `register.ps1` again.

## Remove

```powershell
powershell -ExecutionPolicy Bypass -File .\unregister.ps1
```

This restores the Windows photo handler, removes the extra detail lists, unregisters the Date taken property, and restarts Explorer.

## Project files

| File | Role |
|---|---|
| `NefPropHandler.cpp` | Property handler. Reads the TIFF/EXIF header and fills the empty details. |
| `nefprop.h` | COM class id, and the Windows photo handler it chains to |
| `NefPropHandler.def` | Exports `DllGetClassObject` and the register entry points |
| `nef-datetaken.propdesc` | Date taken property whose text includes seconds |
| `register.ps1` / `unregister.ps1` | Install and remove |
| `build.bat` | Release build |
| `test_handler.cpp` | Prints the properties for one file |

Pixel size and bit depth still come from `PhotoMetadataHandler.dll` when that handler returns them. This code only fills a field the Windows handler left empty, then adds camera, lens, exposure, date, and GPS.

## Time and place

Date taken is the real instant. EXIF `DateTimeOriginal` is the camera clock. If the file has `OffsetTimeOriginal`, that offset is applied. Otherwise the camera clock is treated as the PC's local time. Explorer then shows the result in the PC time zone. A shot taken at 08:45:57 with the camera set to UTC+10 is 06:45:57 on a PC set to UTC+8.

Latitude and longitude are stored as absolute degrees, minutes, and seconds, with the hemisphere in the reference field. South and west are not stored as negative degrees. Altitude is meters, with the EXIF above/below-sea-level flag.

## Limits

- `.nef` and `.nrw` only. JPEG, HEIC, and other raw types are unchanged.
- The folder column named Date taken is still Windows' own field, so that column stops at the minute. The details pane and **Properties → Details** show the seconds.
- High Efficiency NEF (HE / HE*) is still a TIFF container. The EXIF block is read when it sits in the normal IFD. The Raw Image Extension still does not decode those images.
- The handler reads the header and the small EXIF and GPS values. It does not decode the raw image.
- Replacing the property handler means Explorer will not write a star rating into the NEF through this handler.

## After a code change

Close Explorer, run `build.bat`, then start Explorer again. Run `register.ps1` again only if the DLL has moved or the detail lists changed. Explorer loads the DLL from the path stored at install time.
