# Validation et limites

Base source : firmware interne `.34/.35`, Outlander PHEV MY2020, ESP32-C6-DevKitC-1U 8 Mo.

- `.34` : commande HA chauffage 10 minutes, ACK et retour actif/heat/10, confirmation physique de l'utilisateur. Arrêt HA environ cinq minutes plus tard, ACK et retour inactif. Wi-Fi coupé, Zigbee actif à la fin.
- `.35` : lecture au démarrage et retour au repos validés ; conservation d'ACK après perte TCP et exclusion SOC pendant HVAC ajoutées et testées côté hôte. Pas de nouveau cycle de chauffage physique sur cette version.
- `0.1.0-beta.1` : configuration MAC/NVS générique, texte maintenance, dépendances figées et packaging public. Tests/compilation consignés par la CI et les résultats de préparation, **pas un nouveau test physique**.
- `0.1.0-beta.2-dev` : documentation du cas Samsung et inscription directe ESP manuelle via AP. Scénarios init/VIN/ACK/XOR, places pleines, coupures, annulation, timeout et absence de rejeu couverts côté hôte. **Inscription native ESP et nouvelles routes web non testées physiquement**, aucun flash de la carte en service ni demande d'inscription à la voiture. La beta.1 publiée reste inchangée.

Couverture logicielle : protocole deux variantes, fragments/XOR, écritures partielles/FIFO expirée, reconnexion sans rejeu, sessions/cache/horloge, maintenance/OTA, ZCL, résultat atomique, observateur TCP, identité configurable, convertisseur groupé et templates HA natifs. Les mocks ne prouvent pas l'interopérabilité RF en conditions réelles.

Non validé physiquement : autre année de véhicule/carte, refroidissement/désembuage, durées 20/30 minutes, fonctionnement plusieurs jours, installation électrique permanente. Aucune garantie de compatibilité ni de sûreté constructeur. GPS, essence, consommation et LoRa hors périmètre.

Dernier SOC connu pendant les commandes HVAC : certains retours batterie transitoires ont été observés autour du chauffage ; les sessions de commande ne remplacent pas le cache SOC et ne rafraîchissent pas son âge. Une lecture explicite/quotidienne peut toujours recevoir une anomalie du véhicule ; aucune valeur n'est inventée. Cache vide après reboot, expiration 25 h ; exemple HA destiné au pilotage n'utilise le SOC que s'il a moins de 15 minutes et une preuve de rapport récent.

Rapport amont du SOC transitoire : https://community.home-assistant.io/t/my-mitsubishi-phev-integration-via-mqtt/321465/13 .
