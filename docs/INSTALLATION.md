# Installation

## Carte

ESP32-C6-DevKitC-1/1U 8 Mo, alimentation USB stable, antenne adaptée à la carte et proche du véhicule. GPIO2 vers GND pour appairage Zigbee (attention : cette action peut effacer l'association si déjà associé), GPIO3 vers GND pour portail. Vérifier les niveaux 3,3 V ; ne pas appliquer 5 V aux GPIO. Ne pas alimenter directement depuis le 12 V voiture. D'autres C6 nécessitent de vérifier flash, LED RGB et brochage avant compilation.

## Identité Wi-Fi voiture

La carte ne peut pas deviner une identité acceptée par la voiture. Obtenir une **MAC cliente déjà inscrite sur votre véhicule** par la procédure constructeur/outils appropriés. L'outil Go [phev2mqtt](https://github.com/buxtronix/phev2mqtt) documente `client register` ; consulter sa documentation de la version utilisée. Ce firmware ne réalise ni inscription ni suppression de clients. Ne pas utiliser de MAC appartenant à quelqu'un d'autre.

Déconnecter le téléphone ou adaptateur d'origine dont la MAC est utilisée et fermer l'application Mitsubishi avant le test. Une seule station active avec la même MAC ; ne jamais garder l'adaptateur d'origine connecté en parallèle.

Sans configuration NVS, le portail est proposé au démarrage. À défaut utiliser GPIO3. Réseau `PHEV-C6-<suffixe>`, clé `Phev<suffixe>` indiquée sur la console USB. Ouvrir `http://192.168.4.1` ; saisir SSID REMOTE, clé et MAC au format `AA:BB:CC:DD:EE:FF`. Ceci est un format, pas une identité utilisable. Enregistrer puis laisser la carte redémarrer. La saisie de l'identité est réservée au mode AP.

Les clés existantes `ssid`, `pass`, `mac`, `home_ssid`, `home_pass`, `ota_pass` et l'association Zigbee sont conservées lors d'un simple flash d'application. Aucun remplacement forcé de MAC. Ne pas effacer la flash pour une mise à jour ordinaire.

## Zigbee2MQTT

Installer `zigbee2mqtt/outlander_phev_c6.mjs` via la gestion des convertisseurs externes de Z2M. Format validé avec Z2M 2.14.2 ; une autre version peut nécessiter adaptation. Autoriser l'appairage sur le coordinateur puis GPIO2 côté carte. Nom conseillé `Outlander-PHEV-Remote` pour les exemples HA. Les 28 endpoints doivent être présents ; après mise à jour, réinterviewer/reconfigurer si nécessaire avant d'effacer une association.

## Maintenance et OTA

En AP, enregistrer SSID maison, sa clé et un code maintenance/OTA d'au moins 12 caractères. Choisir « Maintenance Wi-Fi maison ». Ouvrir `http://phev-c6.local` ou l'IP obtenue du routeur ; compte `admin`, code configuré. OTA : charger uniquement `firmware.bin` **d'application C6 de ce projet**. Jamais `firmware.factory.bin`, partitions, bootloader ou T-Beam. Conserver l'alimentation et attendre le redémarrage normal.

Le token MQTT `maintenance_mode: "wifi-seb"` reste un alias historique de **Wi-Fi maintenance configuré**, pas un SSID imposé. Modes : normal/AP/Wi-Fi maison. Zigbee et connexion voiture suspendus en maintenance ; retour via page web, reset ou 30 minutes. Une maison inaccessible mène à l'AP.

## Utilisation et dépannage

Actualiser une fois, attendre la fin de session et lire résultat/âge des données. Repos Wi-Fi OFF est normal. Pour HVAC, choisir mode/durée puis transmettre une demande explicite ; aucun statut hors ligne ne doit bloquer le bouton qui ouvre justement la session. Vérifier le fonctionnement réel au premier essai.

En échec : RSSI/antenne, REMOTE visible, client d'origine déconnecté, identité enregistrée, firmware/convertisseur compatibles, association/reconfiguration des endpoints. Ne pas répéter automatiquement une commande après timeout : vérifier l'état réel avant nouvel essai. La compatibilité TCP utilise des API internes lwIP : conserver la plateforme figée, retester avant changement de SDK.
