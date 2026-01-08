## INTRODUCTION

Wine qu'ei un programa qui permet d'executar los logiciaus escriuts entà
Microsoft Windows (comprenent los executables DOS, Windows 3.x, Win32 e
Win64) sus un sistèma Unix. Qu'ei compausat d'un cargador qui carga e
qu'executa un binari Microsoft Windows, atau com d'ua bibliotèca (aperada
Winelib) qui implementa los aperets de l'API de Windows dab l'ajuda deus lors equivalents Unix, X11 o Mac. Aquera bibliotèca que pòt egaument estar
utilizada entà portar còdi Windows cap a un executable Unix natiu.

Wine qu'ei un logiciau liure, distribuït devath GNU LGPL ; legetz lo fichèr
LICENSE entà mei de detalhs.

Wine qu'ei un logiciau liure, distribuït devath GNU LGPL ; legetz lo fichèr
LICENSE entà mei de detalhs.


## AVIADA RAPIDA

Dens lo dossièr rasic deu còdi sorsa de Wine (qui contien aqueth fichèr),
que  lançatz :

```
./configure
make
```

Entà executar un programa, que ` picatz` wine programa . Entà informacions
complementàrias e la resolucions de problèmas, que legetz la seguida
d'aqueth fichèr, las paginas de manuau de Wine, e sustot las nombrosas
informacions qui trobaratz sus https://www.winehq.org.


## CONFIGURACION NECESSÀRIA

Entà compilar e executar Wine, que devetz dispausar d'un deus sistèmas
d'espleitacion seguents :

- Linux version 2.0.36 o ulterior
- FreeBSD 12.4 o ulterior
- Solaris x86 9 o ulterior
- NetBSD-current
- Mac ÒS X 10.8 o ulterior

Per'mor que Wine necessita ua implementacion de las « threads »
(procediments leugèrs) au nivèu deu nuclèu, sols los sistèmas d'espleitacion
mencionats ací dessús que son suportats . Autes sistèmas d'espleitacion
implementant las threads nuclèu que seràn dilhèu pres en carga dens lo
futur.

**Informacions FreeBSD** :
 Wine ne foncionarà generaument pas plan dab las versions FreeBSD
 anterioras a 8.0. Vedetz https://wiki.freebsd.org/wine entà mei
 d'informacions.

**Informacions Solaris** :
 Qu'ei mei que probable que devètz bastir Wine dab la cadena
 d'atrunas GNU (gcc, gas, etc.). Atencion : installar gas n'assegura pas
 que serà utilizat per gcc. Recompiler gcc après l'installacion de gas
 o crear un ligam simbolic de cc, as e ld cap a las atrunas GNU
 corresponents que sembla necessari .

**Informacions NetBSD** :
 Qu'Asseguratz que las opcions USAR_LDT, SYSVSHM, SYSVSEM e SYSVMSG que
  son activadas dens lo vòste nuclèu .

**Informacions Mac ÒS X** :
 Xcode 2.4 o ulterior que hè besonh entà compilar Wine devath x86.
 Lo pilòte Mac que requereish ÒS X 10.6 o ulterior e ne poirà pas estar
 compilat devath 10.5.

**Sistèmas de fichèrs pres en carga** :
 Wine que deuré foncionar sus la màger part deus sistèmas de fichèrs.
 Daubuns problèmas de compatibilitat que son estats raportats pendent
 l'utilizacion de fichèrs accedits per Samba. En mei, NTFS ne horní pas
 totas las foncionalitats de sistèma de fichèrs necessaris entà
 daubuns aplicacions. L'utilizacion d'un sistèma de fichèrs Unix natiu
 qu'ei recomandada .

**Configuracion de basa requerida** :
 Los fichèrs d'entèsta de X11 (aperats xorg-dev devath Debian e
 libX11-devel devath Red Hat) que deven estar installats.
 Ben entenut, qu'auratz besonh deu programa « make » (plan probablament
 GNU make).
 flex 2.5.33 o ulterior, atau com bison, que son egaument requerits .

**Bibliotècas opcionaus** :
 « configura » aficha deus messatges quan bibliotècas opcionaus
 son pas detectadas suu vòste sistèma.
 Consultatz https://gitlab.winehq.org/wine/wine/-/wikis/building-wine
 (en anglés) entà indicacions suus paquets logiciaus qui 
 deuretz installar. Sus las platafòrmas 64 bits, si compilatz
 Wine entau mòde 32 bits (mòda per defaut), las versions 32 bits d'aqueras
  bibliotècas que deven estar installadas.
  
  
## COMPILACION

Entà compilar Wine, que  lançatz :

```
./configure
make
```

Aquò que va bastir lo programa « wine », atau com nombrós binaris e
bibliotècas de supòrt.
Lo programa « wine » carga e qu'executa los executables Windows.
La bibliotèca « libwine » (àlias « Winelib ») que pòt estar utilizada entà
compilar e ligar deu còdi sorsa Windows devath Unix.

Entà véder las opcions de compilacion, que ` picatz ./configura --help`.

Entà mei d'informacion que consultatz https ://gitlab.winehq.org/wine/wine/-/wikis/building-wine

## INSTALLACION

Un còp Wine que basteish corrèctament, `make install` qu'installa
l'executable wine, las bibliotècas associadas, las paginas de manuau de
Wine e quauques autes fichèrs necessaris.

N'oblidatz pas de desinstallar totas las installacions precedentas :
ensajatz `dpkg -r wine`, `rpm -e wine` o `make uninstall` abans d'installar
  ua version navèra.

Un còp l'installacion acabada, que podetz lançar l'atruna de
configuracion `winecfg`. Consultatz la seccion Supòrt de
https://www.winehq.org/ entà obtiéner astúcias de configuracion.


## EXECUTAR DEUS PROGRAMAS

A l'aviada de Wine, que podetz especificar lo camin entièr cap a
l'executable o solament lo nom de fichèr.

Per exemple entà executar lo blòc-que  nòtas :

```
wine notepad (en utilizant lo camin d'accès especificat
wine notepad.exe dens la basa de registre entà localizar
 lo fichèr)

wine c:\\windows\\notepad.exe
 (en utilizant la sintaxi de fichèrs DOS)

wine ~/.wine/drive_c/windows/notepad.exe
 (en utilizant la sintaxi Unix)

wine notepad.exe lisezmoi.txt
 (en aperant lo programa dab paramètres)
```

Wine n'ei pas perfèit, e daubuns programas que pòden donc plantar. Si
aquò que's produseish, qu'obtieneratz un jornau de l'esglachament qui ei recomandat d'estacar a un eventuau rapòrt d'ariçon.


## HONTS D'INFORMACIONS COMPLEMENTÀRIAS

- **WWW** :	Hèra informacions a perpaus de Wine que son disponiblas sus
 WineHQ (https://www.winehq.org) : divèrs guidas, basa de dadas
 d'aplicacions, seguit d'ariçons. Qu'ei probablament lo punt
 melhor de partença .

- **FAQ** : La Hèira a las Questions de Wine que's tròba sus https ://gitlab.winehq.org/wine/wine/-/wikis/FAQ

- **Wiki** : Lo wiki Wine qu'ei situat sus https://gitlab.winehq.org/wine/wine/-/wikis/

- **Gitlab**: Lo desvolopament de Wine qu'ei aubergat sus https://gitlab.winehq.org

- **Listas de difusion** :
        Qu'existeish mantua lista de difusion entaus utilizators e
        los desvolopaires Wine ; vedetz https://gitlab.winehq.org/wine/wine/-/wikis/forums entà en mei
        amplas informacions.

- **Ariçons** :
 Senhalatz los ariçons suu Bugzilla Wine a https://bugs.winehq.org
 Mercés de verificar abans tot dens la basa de dadas de bugzilla
 que lo problèma ei pas dejà conegut, o corregit.

- **IRC** : L'ajuda en linha qu'ei disponibla per la canau `#WineHQ` sus
 irc.libera.chat.