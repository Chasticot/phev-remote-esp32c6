# Vérification de l'ajout inscription ESP — 2026-10-08

Sources `0.1.0-beta.2-dev`. Aucun port série ouvert, aucun flash de carte, aucune inscription/commande véhicule, aucune modification de HA ou de Z2M lors de cette préparation.

- Hôte C++ : 8 suites passent, protocole **43/43** dans chacune des deux variantes (avec/sans `ARDUINO_ARCH_ESP32`). Inclut 12 nouveaux scénarios d'inscription : démarrage exclusivement explicite, blocage diagnostic passif, init + VIN complet, places pleines, ACK corrélé, XOR séquentiel, EOF avant/après demande, échec de connect, deadline sans Wi-Fi/horloge débordée, timeout/annulation, deux corrections BB maximum, TX bloquée sans rejeu.
- Python : **29/29**, dont 7 nouveaux tests de câblage des routes. Ils vérifient dans les sources les gardes AP/interface/admin/CSRF, confirmation, intention RAM, exclusion des opérations concurrentes et adoption NVS séparée. **Ce ne sont pas des essais HTTP sur un ESP réel.**
- JavaScript de la vraie page embarquée : exécuté avec DOM/fetch simulés, tous les résultats et flags `busy`, activation conditionnelle d'adoption, texte non interprété en HTML et gestion du portail inaccessible.
- Convertisseur Z2M : tests passent, toujours 28 endpoints et paramètres de commande groupés.
- Home Assistant : **32/32**, y compris templates Jinja natifs ; aucune modification nécessaire des entités ou scripts.
- Compilation PlatformIO `phev_remote_c6` sur plateforme figée `55.03.312` : succès ; compilation seulement, sans téléversement.
- La CI exécute les mêmes suites et compile le firmware. Les artefacts de `main` sont de développement ; la release `v0.1.0-beta.1` et son tag sont conservés.

Reste à vérifier avec accord du propriétaire : routes HTTP/admin/CSRF sur carte réelle, reconnexion du téléphone à l'AP après changement de canal, inscription native acceptée en mode constructeur, maintien du téléphone déjà inscrit, lecture normale après adoption puis contact OFF. Ne pas présenter ces étapes comme validées à partir des mocks.
