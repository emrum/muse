![Alt tag](muse.png)

MusE  5.0 Beta 
============================
MusE is a MIDI/Audio sequencer with recording and editing capabilities written originally by 
 Werner Schweer now developed and maintained by the MusE development team. 
MusE aims to be a complete multitrack virtual studio for Linux.
It is published under the GNU General Public License. 

Visit the original MusE web site at: https://muse-sequencer.github.io/


## THIS IS A FORK, of original MusE 4.0 that adds these:

### FEATURES 

- full QT6 port (requires qt >= 6.8)
- Qlementine Style Engine (included with bug-fixes)
- smooth canvas scrolling , fast dragging
- CLAP Plugins Synth AND Effects 
- MIDI Ports - automatic naming


### BUG FIXES

- mixer restores level 
- canvas slow dragging
- race conditions 
- XML reader fix
- language-locale fix
- fix midi ports (made more reliable)


### TODO

- minor style fixes (transport width)
- possible hang on template loading ? (scrutinize)
- on-start : popup conflict ("safe current/defaul" vs. "continue" )
- keyboard shortcuts - disabled ?? (scrutinize)


### The FORK IS usable already and appears crash free so far. 


Most Linux distributions include MusE ready to install, check your package manager.

Installation from source code:
------------------------------
Stable source code releases are [here](https://github.com/muse-sequencer/muse/releases).
Or one of the git branches can be cloned, built, and installed.

Installation instructions are in the [README](src/README) file.

Documentation:
--------------
[LICENSE](src/COPYING)
[AUTHORS](src/AUTHORS)

These and other important documents, READMEs, and addendums are in the [src](src) directory.

The official MusE Manual (work in progress) has migrated to the wiki and can be found [here](https://github.com/muse-sequencer/muse/wiki/Documentation).
