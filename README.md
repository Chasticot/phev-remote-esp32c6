# PHEV Remote ESP32-C6

Passerelle **Wi-Fi Mitsubishi Outlander PHEV → Zigbee2MQTT → Home Assistant**, avec configuration AP et mise à jour OTA sur le Wi-Fi local. Version **0.1.0-beta.1**.

Projet indépendant, adapté du travail de rétroingénierie de [buxtronix/phev2mqtt](https://github.com/buxtronix/phev2mqtt). Ce dépôt n'est ni le logiciel Go original ni un produit officiel Mitsubishi. Voir [crédits et licences](NOTICE.md).

## Ce que fait cette version

- Chauffage, refroidissement, désembuage, arrêt ; durées 10/20/30 minutes.
- Batterie de traction, recharge, câble branché, temps de recharge si reçu, verrouillage/accès, éclairage, état et résultat des commandes.
- Connexion voiture uniquement à la demande : première lecture 15 s après démarrage, puis une tentative toutes les 24 h. Session bornée à 60 s, aucun rejeu d'une commande dans une session suivante.
- Au repos : Wi-Fi voiture arrêté, Zigbee actif ; dernières observations conservées avec âge et validité. Un ACK n'est pas une confirmation physique de fonctionnement HVAC.
- AP de configuration et OTA web authentifiée. La maintenance suspend Zigbee et ne se fait pas par OTA Zigbee.

**Pas de CAN, LoRa, GPS, niveau d'essence ni consommation dans ce firmware.**

## Matériel et validation

Profil : **ESP32-C6-DevKitC-1 / 1U, flash 8 Mo**, USB natif, Zigbee ED. Boutons externes GPIO2/GND (appairage Zigbee), GPIO3/GND (portail) ; voir [installation](docs/INSTALLATION.md). Ne pas utiliser ces images sur ESP32 classique, C3, S3 ou T-Beam.

Le firmware source a été testé sur un **Outlander PHEV 2020** : lecture batterie, chauffage 10 minutes et arrêt demandé cinq minutes après, avec confirmation physique du chauffage et retour voiture inactif après arrêt. La configuration générique de cette bêta n'a pas été testée sur un autre véhicule. Voir [validation et limites](docs/VALIDATION.md). Compatibilité autres années/cartes non garantie.

## Installation rapide

1. Compiler ou récupérer l'image **d'application** `firmware.bin` d'une version identifiée. Installer avec PlatformIO sur une carte C6 compatible.
2. Ouvrir l'AP de la carte, puis `http://192.168.4.1`. Configurer votre SSID REMOTE, sa clé et **votre propre MAC cliente déjà enregistrée dans votre voiture**. Aucune MAC ou clé personnelle n'est fournie.
3. Installer `zigbee2mqtt/outlander_phev_c6.mjs` comme convertisseur externe Z2M, activer l'appairage du coordinateur puis celui de la carte.
4. Optionnel : installer le package et les cartes [Home Assistant](homeassistant/README.md).
5. Configurer le SSID/clé de maintenance et un code OTA d'au moins 12 caractères via AP. Les mises à jour suivantes utilisent ce réseau local.

La carte **ne fait pas d'inscription automatique dans la voiture**. Si vous n'avez pas encore d'identité enregistrée, effectuer l'inscription d'un client à l'aide de la procédure Mitsubishi et d'un outil adapté tel que phev2mqtt ; ne pas effacer les inscriptions existantes sans nécessité. Déconnecter le client d'origine pendant l'utilisation de sa MAC. Voir la procédure détaillée et les précautions avant de commander HVAC.

## Compilation et tests

Plateforme pioarduino figée à `55.03.312` (Arduino-ESP32 3.3.12). Installer PlatformIO Core 6.2.0 :

```sh
python -m pip install platformio==6.2.0
pio run -e phev_remote_c6
pio run -e phev_remote_c6 -t upload --upload-port YOUR_PORT
```

Pour les tests logiciels (sans voiture, HA, MQTT ni Zigbee) :

```sh
python -m pip install -r requirements-dev.txt
python -m unittest discover -s tests -p 'test_*.py'
python homeassistant/test_ha_controls.py
node --experimental-vm-modules tests/converter_test.mjs
python tests/run_host.py --compiler g++
```

Windows avec Zig installé : `python tests/run_host.py --compiler zig --zig` ou `./tests/run_protocol.ps1 -Compiler zig`. Node 22+ recommandé. La CI compile le firmware et exécute les tests ; elle ne commande jamais une voiture. Les binaires de CI sont des artefacts temporaires, pas une preuve de validation physique.

## Sécurité

Ne pas exposer le portail à Internet. OTA HTTP locale authentifiée avec contrôle de type d'image et CSRF, **sans TLS ni signature cryptographique**. Le mot de passe initial de l'AP est dérivé du suffixe de la carte : configuration à effectuer sur un réseau/lieu de confiance. Changer le code maintenance généré via AP. Ne jamais publier logs, captures, VIN, MAC personnelles, clés ou sauvegardes NVS. Lire [SECURITY.md](SECURITY.md).

Conserver une copie du firmware qui fonctionne avant mise à jour. Une mise à jour d'application conserve NVS et association Zigbee, mais effacer toute la flash les supprime. Cette variante lit la MAC NVS existante sans la remplacer au démarrage. Ne pas exposer les commandes MQTT avec `retain=true`.

Licence des contributions projet : **GPL-3.0-or-later** ; composant `NetworkClientBounded.cpp` : **LGPL-2.1-or-later** avec mentions d'origine conservées. Les sources correspondant à une image distribuée et les notices sont disponibles dans ce dépôt.
