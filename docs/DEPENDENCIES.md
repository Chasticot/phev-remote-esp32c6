# Dépendances et reproductibilité

Version figée dans `platformio.ini` : pioarduino platform-espressif32 `55.03.312`, Arduino-ESP32 `3.3.12`, ESP-IDF libs `5.5.5+sha.b774170ff46`. PlatformIO Core recommandé/CI : `6.2.0`. Les sous-paquets référencés par cette plateforme sont résolus par PlatformIO. Node 22 ; tests PyYAML 6.0.3, Jinja2 3.1.6. Mettre à jour ces versions exige un nouveau build et des tests, notamment API lwIP internes.

Licences : plateforme pioarduino Apache-2.0 ; Arduino-ESP32 et copie NetworkClient identifiés LGPL-2.1-or-later ; ESP-IDF généralement Apache-2.0 avec composants tiers sous licences propres ; lwIP BSD ; Espressif Zigbee SDK selon ses licences fournies par le paquet. Consulter les notices **des paquets exacts** et les sous-composants avant redistribution d'une image. Bibliothèques/codes SDK non copiés dans ce dépôt.

Références : https://github.com/pioarduino/platform-espressif32/releases/tag/55.03.312 ; https://github.com/espressif/arduino-esp32/tree/3.3.12 ; https://github.com/espressif/esp-idf/tree/v5.5.5 ; https://github.com/espressif/esp-zigbee-sdk .

La CI fournit l'image et un SHA256, utiles pour identifier l'artefact ; cela ne constitue ni une signature de firmware ni une garantie de builds bit-identiques entre machines (horodatages/chemins du SDK peuvent différer). Pour une release, conserver tag source, logs de build, versions exactes et sources correspondantes.
