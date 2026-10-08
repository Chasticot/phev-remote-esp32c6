# Historique

## 0.1.0-beta.2-dev — non publiée en release

- Retour d'expérience anonymisé Samsung Android : MAC physique/privée, EOF TCP et limites de l'explication ; fonctionnement avec une identité Realtek inscrite séparément.
- Inscription native ESP expérimentale via AP local authentifié, CSRF et confirmation sur place ; attente init/VIN MY18, refus des deux places occupées, demande registre 10/01 et suivi ACK.
- Pas d'inscription automatique, d'effacement de clients ni de commande HVAC ; tentative bornée, résultat incertain après perte d'ACK, adoption NVS séparée et explicite. MAC existante conservée par défaut.
- Tests de protocole et gardes web complétés. Aucun essai d'inscription réel ni flash sur la carte en service ; release beta.1 inchangée.

## 0.1.0-beta.1 — 2026-10-08

Première préparation publique, issue du firmware interne `2026-10-08.35-ha-command`.

- Identité MAC configurable dans l'AP, aucune identité prédéfinie ; NVS existante préservée, MAC nulle/multicast rejetée.
- Wi-Fi de maintenance générique sans SSID par défaut ; token historique `wifi-seb` conservé uniquement pour la compatibilité du protocole Zigbee.
- Traitement du message Z2M complet, paramètres de commande atomiques 32 bits, durée HA native numérique et arrêt indépendant du helper durée.
- Sessions à la demande/24 h, TX bornée, ACK TCP cumulatif et pas de rejeu inter-session.
- Résultat ACK conservé après fermeture TCP ; SOC des sessions HVAC non adopté comme nouveau relevé.
- Documentation, licences, tests portables, CI, dépendances figées ; aucune donnée de l'installation d'origine publiée.

Pas de flash de cette variante sur le véhicule durant la préparation publique. Refroidissement/désembuage et durées 20/30 minutes non revalidés physiquement.
