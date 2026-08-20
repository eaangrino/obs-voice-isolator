
> [!IMPORTANT]
> **Project status**
>
> 1. This project is **AI slop**: it was generated and assembled quickly with the help of artificial intelligence, but its first iteration worked as expected.
> 2. The project is still in the testing phase. It is currently working well with a single microphone based on the tests performed so far.
> 3. The two-microphone feature is still experimental and currently requires manual adjustments depending on the devices, latency, and environment.

# OBS Voice Isolator

A native audio filter for OBS Studio, based on the official `obsproject/obs-plugintemplate`.

## What it does

1. **RNNoise** reduces stationary noise and some types of variable noise.
2. A **voice-probability-driven gate** attempts to let only speech pass through.
3. A heuristic detector attenuates **breathing sounds and non-vocal noise**.
4. It can optionally use a **second microphone as an environmental reference** through an adaptive NLMS filter.

## Technical reality

No filter can guarantee “only voice and absolutely nothing else” across every room, microphone, and voice. Breathing sounds, unvoiced consonants, and certain types of noise share similar acoustic characteristics. This plugin is aggressive: it may cut quiet words, sentence endings, `s`, `f`, and `j` sounds, as well as whispered speech.

The second microphone mainly helps with correlated noise such as fans, air conditioning, computer noise, and street noise. It will not work magically if both microphones have unstable delays, use different drivers, or capture too much of the speaker’s voice.

## Requirements on Windows 11

- OBS Studio x64.
- Visual Studio 2022.
- **Desktop development with C++** workload.
- MSVC v143.
- Windows SDK 10.0.20348 or newer; 10.0.22621 is recommended.
- CMake 3.28 or newer.
- Git.
- Internet access during the initial setup: the template downloads OBS 31.1.1 and `obs-deps`.

Check all requirements:

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\scripts\check-tools.ps1
````

## Build

From PowerShell, in the project root:

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\scripts\build-windows.ps1
```

Clean build:

```powershell
.\scripts\build-windows.ps1 -Clean
```

The output is generated at:

```text
dist\obs-voice-isolator\bin\64bit\obs-voice-isolator.dll
dist\obs-voice-isolator\data\locale\...
```

## Installation

Close OBS and run:

```powershell
.\scripts\install-user.ps1
```

To remove it:

```powershell
.\scripts\uninstall-user.ps1
```

## Configuration in OBS

### One microphone

1. Add your main microphone to OBS.
2. Open **Filters**.
3. Under audio filters, add **Voice Isolator (aggressive)**.
4. Speak normally, quietly, and loudly while adjusting:

   * **Voice probability threshold**: higher values remove more noise, but may cut more of your voice.
   * **Breath suppression**: `0.90–1.00` is very aggressive.
   * **Release / tail**: `100–200 ms` helps preserve the endings of words.
   * **Noise floor**: start at `-58 dB`.

### Two microphones (Experimental)

1. The microphone closest to your mouth should be the main source.
2. Add the second microphone as another **Audio Input Capture** source.
3. In the main microphone filter, select that source under **Auxiliary environment microphone**.
4. Place the auxiliary microphone farther from your mouth and closer to the noise source.
5. Start with:

   * Auxiliary cancellation: `0.50–0.70`.
   * Adaptation: `0.02–0.05`.
   * Offset: `0 ms`.
6. While remaining silent, let the environmental noise play for several seconds so the filter can adapt.
7. If the result becomes worse or sounds metallic, disable the auxiliary microphone or adjust the offset between `-50` and `+50 ms`.

Do not send the auxiliary microphone to the recording or stream if you only want to use it as a reference. Mute its tracks or output according to your OBS configuration, but do not prevent OBS from capturing it internally.

## Logging and diagnostics

In OBS, open:

```text
Help → Log Files → View Current Log
```

Search for:

```text
[obs-voice-isolator]
```

If the filter does not appear, check:

```text
%APPDATA%\obs-studio\plugins\obs-voice-isolator\bin\64bit\obs-voice-isolator.dll
%APPDATA%\obs-studio\plugins\obs-voice-isolator\data\locale\es-ES.ini
```

## License

GPL-2.0-or-later, compatible with the template and OBS Studio.

