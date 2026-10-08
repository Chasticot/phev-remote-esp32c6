# Exemple Home Assistant

Le package `PHEV-HA-controls.yaml` contient les helpers, templates de fraîcheur, scripts de commandes et maintenance. Il n'est pas une sauvegarde d'une installation personnelle. **Sauvegarder sa configuration avant toute fusion.**

Prérequis : Home Assistant avec Zigbee2MQTT et son intégration MQTT, convertisseur du dépôt installé, appareil nommé **Outlander-PHEV-Remote** dans Z2M. Si votre friendly_name/base topic/entity_id diffère, adapter le package et le dashboard. Les noms d'entités supposés sont `sensor.outlander_phev_remote_session_word`, les autres capteurs `outlander_phev_remote_*` générés par Z2M ; vérifier dans Outils de développement → États.

Installation : copier le package sous `config/packages/phev.yaml` si les packages sont déjà activés, sinon fusionner sous les clés existantes sans les remplacer. Valider la configuration avec HA avant recharge/redémarrage. Pour le dashboard, ajouter manuellement la vue de `PHEV-dashboard-view.yaml` ; ce fichier n'est pas une configuration complète de dashboard.

Les scripts publient des demandes non retenues, attendent un **nouvel ID/résultat atomique** et n'effectuent pas de répétition. Chauffage/refroidissement/désembuage : 10/20/30 min ; arrêt reste possible si le helper de durée est indisponible. La preuve d'un ACK ne remplace pas la vérification de l'état HVAC réel. Aucun chauffage programmé automatiquement.

Le latch de rapport batterie récent est volontairement conservateur après redémarrage : une simple valeur MQTT retained ne prouve pas un nouveau relevé. Utiliser Actualiser les infos puis attendre un nouveau rapport avant de piloter une autre automatisation par SOC. Le capteur utilisable est limité à 15 minutes ; ne pas brancher automatiquement un système de recharge sans valider votre logique propre.

Maintenance : token historique `wifi-seb` signifie Wi-Fi maison configuré, quel que soit son SSID. Zigbee suspendu en maintenance ; retour par portail/reset/délai automatique. Notifications des demandes désactivées par défaut. Aucune automatisation Tempo, autre voiture ni limite de courant n'est incluse.

Test : installer les dépendances du dépôt puis `python homeassistant/test_ha_controls.py` ; tous les tests Jinja doivent s'exécuter, aucun ne doit être ignoré.
