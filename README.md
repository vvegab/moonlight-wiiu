# Moonlight Wii U

Moonlight Wii U is a port of [Moonlight Embedded](https://github.com/moonlight-stream/moonlight-embedded), which is an open source client for [Sunshine](https://github.com/LizardByte/Sunshine) and NVIDIA GameStream. Moonlight allows you to stream your full collection of games and applications from your PC to other devices to play them remotely.

## Jugar 3DS con Citra

Esta versión recorta un solo stream de Sunshine. La pantalla superior de la 3DS va a la TV y la táctil al GamePad. No hace falta un segundo cliente ni modificar Sunshine.

### En el PC

1. Instala [Sunshine](https://github.com/LizardByte/Sunshine) y [Nefarius Virtual Gamepad](https://github.com/nefarius/ViGEmBus/releases/latest). Reinicia el PC después de ViGEm: sin ese driver el GamePad no llega a Citra.
2. En Citra Nightly deja el layout **Large Screen**, proporción 4, pantalla chica abajo a la derecha y sin intercambiar pantallas. Es el valor por defecto (`layout_option=2`).
3. Maximiza Citra en un escritorio de 1920×1080, tal como se va a capturar. Las regiones de `moonlight.conf` incluyen la barra de título y la barra de tareas. Si mueves la ventana, o si Sunshine captura solo la ventana de Citra, el recorte deja de coincidir.
4. En los controles de Citra, asigna el mando **Xbox 360** que crea Sunshine a los botones de la 3DS y al circle pad. Moonlight ya envía ese mando. La opción `citra_input` traduce el GamePad a teclado y está desactivada: actívala solo si Citra no tiene perfil de mando, porque si no las dos entradas se suman.
5. En Sunshine, la aplicación que se abre es el escritorio. `moonlight.conf` ya pide `app = Desktop`, que es el nombre por defecto de esa captura. Si en Sunshine la renombraste, usa ese mismo nombre.

### En la Wii U

El paquete va en `sd:/wiiu/apps/moonlight/`. Tiene que incluir `moonlight.rpx` (o `moonlight.wuhb`) y `moonlight.conf`.

En la configuración, descomenta `address` y pon la IP del PC. El bloque que ya viene activo es:

```
width = 1280
height = 720
fps = 30
bitrate = 5000
dualscreen = true
tv_region = 0,0.041739,0.833984,0.933913
drc_region = 0.833984,0.711304,1,0.933913
mouse_mode = absolute
app = Desktop
```

`tv_region` es la pantalla grande de la izquierda. `drc_region` es la táctil de abajo a la derecha. `mouse_mode = absolute` hace que el lápiz del GamePad pulse esa zona.

Empareja Moonlight con Sunshine, acepta el PIN en el PC y entra a la sesión. Citra tiene que estar maximizado antes de conectar.

### Si se corta la conexión

Por Wi‑Fi, 10000 kbps llena el enlace de la Wii U. La imagen se ve bien, pero Sunshine deja de recibir los ACK y corta a los 30 segundos con `Connection terminated with error: -1`. 5000 kbps a 720p y 30 fps deja margen. Sube el bitrate solo si usas el adaptador LAN.

## Quick Start

> :information_source: A Wii U LAN Adapter is recommended!

* Grab the latest version from the [releases page](https://github.com/GaryOderNichts/moonlight-wiiu/releases) and extract it to the root of your SD Card.
* Enter the IP of your Sunshine/GFE server in the `moonlight.conf` file located at `sd:/wiiu/apps/moonlight`.
* Ensure your Sunshine/GFE server and Wii U are on the same network.
* If using GFE, turn on Shield Streaming in the GFE settings.
* Pair Moonlight Wii U with the server.
* Accept the pairing confirmation on your PC.
* Connect to the server with Moonlight Wii U.
* Play games!

## Configuration

You can configure all of the documented settings in the `moonlight.conf` file located at `sd:/wiiu/apps/moonlight`.  
Note that a lot of option are commented out by default, to edit them you need to remove the `#` in front of them.

## Supported controllers

* Gamepad (can be disabled with the `disable_gamepad` option).
* Up to 4 Wii U Pro Controllers and Wii Classic Controllers (Pro).  
  The Gamepad needs to be disabled to use the 4th controller.

## Troubleshooting
### Input doesn't work when using Sunshine
Verify that you've installed [Nefarius Virtual Gamepad](https://github.com/nefarius/ViGEmBus/releases/latest) and restarted your PC after the installation.

### The stream disconnects frequently/immediately
Depending on your network connection you need to adjust the configuration to find a stable bitrate and resolution.
Try something like this to get started:
```
width = 854
height = 480
fps = 30
```
```
bitrate = 1500
```
Then slowly increase the bitrate until the stream is no longer stable.

### Can't find app Steam
Moonlight Wii U tries to start the app "Steam" by default, but sunshine does not have a default Application called "Steam".  
You can either rename the app in the `moonlight.conf` to
```
app = Steam Big Picture
```
which is a default option in sunshine or add a new application called "Steam" in the sunshine configuration.

## See also

[Moonlight-common-c](https://github.com/moonlight-stream/moonlight-common-c) is the shared codebase between different Moonlight implementations

## Contribute

1. Fork us
2. Write code
3. Send Pull Requests

## Building from source
Install the required dependencies: `(dkp-)pacman -S --needed wiiu-dev wiiu-sdl2 wiiu-curl wiiu-mbedtls ppc-freetype ppc-libopus ppc-libexpat`.  
Run `make` to build moonlight.

### Using docker
You can also build moonlight-wiiu using the provided Dockerfile.  
Use `docker build -t moonlightbuilder .` to build the container.  
Then use `docker run -it --rm -v ${PWD}:/project moonlightbuilder make` to build moonlight.  
