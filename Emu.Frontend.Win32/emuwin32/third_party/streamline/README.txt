NVIDIA Streamline 2.14.1's headers, as they come in the SDK's include\ folder, under the MIT
licence beside them (LICENSE.txt) - so the emulator builds with DLSS support whether or not the SDK
has been fetched.

Only the headers are here. Streamline's DLLs, and DLSS's own (nvngx_dlss.dll), are NVIDIA's under
NVIDIA's RTX SDK licence: they are fetched into Temp\streamline\ by ..\fetch_streamline.ps1, never
committed, and copied beside the executable by the build. See Docs/DLSS-Plan.md, phase 4.
