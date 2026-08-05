> [!IMPORTANT]
> **Estado del proyecto**
>
> 1. Este proyecto es **IA slop**: fue generado y ensamblado rápidamente con ayuda de inteligencia artificial, pero en su primera iteración funcionó como se esperaba.
> 2. El proyecto continúa en fase de pruebas. Con un solo micrófono está funcionando correctamente según las pruebas realizadas hasta ahora.
> 3. La función para usar dos micrófonos todavía está en pruebas y, de momento, requiere ajustes manuales según los dispositivos, la latencia y el entorno.


# OBS Voice Isolator

Filtro nativo para OBS Studio, basado en la plantilla oficial `obsproject/obs-plugintemplate`.

## Qué hace

1. **RNNoise** reduce ruido estacionario y parte del ruido variable.
2. Una **puerta guiada por probabilidad de voz** intenta dejar pasar únicamente habla.
3. Un detector heurístico atenúa **respiraciones y ruido no vocal**.
4. Opcionalmente utiliza un **segundo micrófono como referencia ambiental** mediante un filtro adaptativo NLMS.

## Verdad técnica

No existe un filtro que garantice “solo voz y absolutamente nada más” en todos los cuartos, micrófonos y voces. Respiraciones, consonantes no sonoras y ciertos ruidos comparten características acústicas. Este plugin es agresivo: puede recortar palabras suaves, finales de frases, `s`, `f`, `j` y voz susurrada.

El segundo micrófono ayuda principalmente con ruido correlacionado —ventilador, aire acondicionado, PC, calle—. No funciona como magia si ambos micrófonos tienen retrasos inestables, drivers distintos o capturan mucha voz.

## Requisitos en Windows 11

- OBS Studio x64.
- Visual Studio 2022.
- Carga de trabajo **Desktop development with C++**.
- MSVC v143.
- Windows SDK 10.0.20348 o más reciente; recomendado 10.0.22621.
- CMake 3.28 o superior.
- Git.
- Internet durante la primera configuración: la plantilla descarga OBS 31.1.1 y `obs-deps`.

Comprueba todo:

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\scripts\check-tools.ps1
```

## Build

Desde PowerShell, en la raíz:

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\scripts\build-windows.ps1
```

Build limpio:

```powershell
.\scripts\build-windows.ps1 -Clean
```

El resultado queda en:

```text
dist\obs-voice-isolator\bin\64bit\obs-voice-isolator.dll
dist\obs-voice-isolator\data\locale\...
```

## Instalación

Cierra OBS y ejecuta:

```powershell
.\scripts\install-user.ps1
```

Para eliminarlo:

```powershell
.\scripts\uninstall-user.ps1
```

## Configuración en OBS

### Un micrófono

1. Agrega tu micrófono principal a OBS.
2. Abre **Filtros**.
3. En filtros de audio, agrega **Aislador de voz (agresivo)**.
4. Habla normal, bajo y fuerte mientras ajustas:
   - **Umbral de probabilidad de voz**: mayor elimina más, pero corta más voz.
   - **Supresión de respiración**: `0.90–1.00` es muy agresivo.
   - **Liberación / cola**: `100–200 ms` conserva finales de palabras.
   - **Piso de ruido**: empieza en `-58 dB`.

### Dos micrófonos

1. El micrófono cercano a tu boca es la fuente principal.
2. Agrega el segundo como otra fuente **Captura de entrada de audio**.
3. En el filtro del micrófono principal, selecciona esa fuente en **Micrófono auxiliar de ambiente**.
4. Coloca el auxiliar lejos de la boca y más cerca del ruido.
5. Empieza con:
   - Cancelación auxiliar: `0.50–0.70`.
   - Adaptación: `0.02–0.05`.
   - Desfase: `0 ms`.
6. En silencio, deja el ruido sonar varios segundos para que el filtro se adapte.
7. Si empeora o produce sonido metálico, desactiva el auxiliar o ajusta el desfase entre `-50` y `+50 ms`.

No envíes el micrófono auxiliar a la grabación o stream si solo lo quieres como referencia: silencia sus pistas/salida según tu configuración, pero no lo silencies internamente antes de que OBS lo capture.

## Registro y diagnóstico

En OBS abre:

```text
Ayuda → Archivos de registro → Ver registro actual
```

Busca:

```text
[obs-voice-isolator]
```

Si el filtro no aparece, comprueba:

```text
%APPDATA%\obs-studio\plugins\obs-voice-isolator\bin\64bit\obs-voice-isolator.dll
%APPDATA%\obs-studio\plugins\obs-voice-isolator\data\locale\es-ES.ini
```

## Licencia

GPL-2.0-or-later, compatible con la plantilla y con OBS Studio.
