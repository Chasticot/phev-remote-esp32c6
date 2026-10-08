# Sécurité et usage responsable

Logiciel expérimental destiné au véhicule appartenant à l'utilisateur. Ne pas l'utiliser sur un véhicule tiers sans autorisation. Ne pas démarrer le véhicule, neutraliser une protection ou injecter des trames CAN : ces fonctions n'existent pas ici.

- Tester à l'arrêt, vérifier physiquement les premières commandes et conserver l'application constructeur comme solution de secours. Le résultat ACK et la dernière observation HVAC sont distincts.
- Aucune préclimatisation automatique au démarrage ni demande conservée entre sessions. Les commandes MQTT doivent toujours être non retenues. Le broker/coordonateur/HA doivent être sécurisés.
- Aucune identité/clé embarquée : configuration stockée en NVS. La NVS n'est pas chiffrée par cette application ; l'accès physique à la carte peut exposer les clés.
- Portail et OTA uniquement sur le réseau local de confiance, jamais de redirection de port Internet. HTTP n'apporte pas de confidentialité sur ce réseau. Images contrôlées mais non signées.
- La configuration de l'identité voiture est autorisée seulement dans l'AP local. Le mot de passe AP dérivé du suffixe n'est pas une protection contre un adversaire déterminé.
- L'inscription expérimentale native ESP demande AP/interface locale, code admin, CSRF et confirmation sur place ; aucune inscription au boot ni via Zigbee, aucun effacement de clients. Une perte d'ACK après envoi donne un résultat incertain : vérifier avant de relancer. La MAC existante reste sauvegardée jusqu'à adoption explicite ; voir [procédure et limites](docs/MAC-ET-INSCRIPTION.md).
- Maintenance suspend Zigbee ; retour normal via portail/reset/délai 30 minutes. Impossible de compter sur une commande Zigbee d'arrêt tant que la maintenance est active.
- Ne pas mettre dans une issue de VIN, MAC personnelle, SSID complet, IP privée propre à votre maison, clé Wi-Fi, token HA/MQTT ou capture brute. Fournir un journal expurgé, version, matériel et symptômes.

Pour un défaut de sécurité, contacter le mainteneur sans publier d'identifiants ni procédure visant un véhicule réel. Utiliser le signalement privé GitHub si disponible ; sinon demander un canal de contact sans détailler publiquement l'exploitation. La bêta n'a pas fait l'objet d'un audit de sécurité indépendant.
