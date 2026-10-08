# Identité Wi-Fi : Samsung Android et inscription directe de l'ESP

## Disponibilité

L'inscription directe est **expérimentale dans les sources `0.1.0-beta.2-dev` sur `main`**. Elle n'existe pas dans les images de la release `v0.1.0-beta.1`. Compiler les nouvelles sources pour obtenir le bouton. Les tests logiciels ne remplacent pas un essai d'inscription sur une voiture : cet essai n'a pas encore été effectué. Aucune nouvelle image n'a été installée sur la carte en service lors de cet ajout. Voir [tests effectués et validation restante](INSCRIPTION-CHECKS.md).

Si votre passerelle fonctionne déjà avec une identité inscrite, **ne changez rien pour cette mise à jour**. L'identité sauvegardée est conservée. L'inscription ne démarre jamais au boot, via Zigbee ou après une coupure de courant.

## Le problème rencontré avec le Samsung

Sur l'Outlander PHEV MY2020 de développement :

- L'application Mitsubishi du Samsung fonctionnait, y compris la préclimatisation.
- Les adresses affichées ont suscité une confusion entre MAC privée du réseau et MAC physique. L'utilisateur a ensuite choisi la MAC physique pour REMOTE et réinscrit son téléphone.
- Copier cette MAC sur l'ESP n'a pas suffi : association Wi-Fi et adresse IP obtenues, mais fermeture TCP après environ 50–65 ms dans le diagnostic passif, sans octet applicatif reçu. Un essai avec ping immédiat avait reçu un ACK de ping, puis une fermeture, sans initialisation complète.
- Une identité d'adaptateur Realtek inscrite séparément avec `phev2mqtt`, puis utilisée seule par l'ESP, a permis la connexion. Des corrections TCP et de coexistence Wi-Fi/Zigbee ont aussi été apportées au cours du développement.

Ces observations **ne démontrent ni une incompatibilité de toutes les MAC Samsung, ni une authentification secrète supplémentaire**. Nous n'avons pas de capture comparative complète permettant d'isoler une cause unique. Un ACK de ping ne prouve pas que le client est autorisé à piloter la voiture ; `recv=0` décrit une fin de flux, pas la raison du refus.

### Ce que fait réellement Android

Android peut utiliser une MAC privée pour l'association Wi-Fi. Avec la randomisation désactivée **pour ce réseau**, il utilise la MAC matérielle ; la voiture ne reçoit pas magiquement cette dernière sous une autre MAC privée. La stabilité de la MAC privée dépend des paramètres et de la version Android. Voir la [documentation Android officielle](https://source.android.com/docs/core/connect/wifi-mac-randomization-behavior).

Sur Samsung, le libellé varie selon One UI : détails du réseau REMOTE → type d'adresse MAC → adresse du téléphone/physique. Modifier ce choix ne réinscrit pas automatiquement un client dans la voiture. Ne désactiver ce réglage que pour REMOTE si nécessaire, pas sur tous les réseaux. Ne jamais connecter simultanément téléphone et ESP avec la même MAC.

La solution proposée pour de nouvelles installations est une **identité stable propre à l'ESP**, inscrite directement, sans clonage du téléphone. Ce choix reste à valider physiquement avec la nouvelle fonction.

## Inscription depuis le portail ESP

### Préparer le portail

1. Installer un firmware construit depuis `main` incluant cette fonction. Conserver un firmware fonctionnel et ne pas effacer toute la flash.
2. Ouvrir l'AP `PHEV-C6-<suffixe>`, puis `http://192.168.4.1` (GPIO3 si nécessaire).
3. Enregistrer le SSID et la clé REMOTE. Pour une installation vierge, consulter **Inscrire directement la MAC native ESP**, recopier la MAC affichée dans la configuration voiture, enregistrer puis revenir en AP. Cela sauvegarde la configuration, **pas une inscription sur la voiture**. Si une MAC fonctionnelle existe déjà, la garder.
4. Configurer votre code maintenance/OTA. La page d'inscription utilise `admin` et ce code ; le code initial est consultable dans la section maintenance de l'AP. La page n'est pas accessible depuis le Wi-Fi maison ou l'interface cliente REMOTE.

### Préparer la voiture, puis lancer une seule tentative

Selon [Mitsubishi — Before You Begin, STEP2/STEP3](https://www.mitsubishi-motors.com/en/products/outlander_phev/app/remote/jizen.html), porte conducteur fermée, passer en ACC et alterner LOCK/UNLOCK sur la télécommande : **10 pressions au total dans les 10 secondes**. La fenêtre d'inscription dure cinq minutes. Le nombre de bips indique les clients déjà inscrits : 1 bip = aucun, 2 = un, 3 = deux ; maximum deux clients. Terminer avec le contact OFF.

**Feux de détresse éteints : ne pas suivre la procédure de suppression/initialisation.** S'il n'y a plus de place, arrêter : cet outil ne supprime aucun client. Libérer une place relève d'une décision séparée du propriétaire et des procédures constructeur.

1. Fermer l'application Mitsubishi ; déconnecter de REMOTE le téléphone, les adaptateurs et les autres passerelles. Le téléphone peut rester connecté à l'AP ESP pour utiliser le portail.
2. Dans la page **Inscrire directement la MAC native ESP**, cocher la confirmation de présence, de mode inscription et de place libre, puis cliquer sur **Inscrire la MAC native de cet ESP**.
3. L'AP peut redémarrer/changer de canal lors de la préparation et de l'association à REMOTE. Se reconnecter à l'AP ESP et recharger `/registration` pour consulter le résultat.
4. Après **ACK reçu**, choisir séparément **Utiliser cette MAC et redémarrer en mode normal**, seulement si vous souhaitez remplacer l'identité actuelle. Le résultat et cette autorisation d'adoption sont en RAM : ils disparaissent au reboot.
5. Mettre la voiture OFF, puis demander une actualisation depuis HA. Une nouvelle connexion avec lecture de télémétrie confirme l'utilisation de l'identité ; un ACK d'inscription seul ne constitue pas cette vérification.

### Ce que le firmware envoie et ses limites

L'implémentation suit [`phev2mqtt client register`](https://github.com/buxtronix/phev2mqtt/blob/8f423405eef2f833a6a90a93a577624c5875a24c/cmd/register.go) et son [client](https://github.com/buxtronix/phev2mqtt/blob/8f423405eef2f833a6a90a93a577624c5875a24c/client/client.go) : connexion, pings/ACK d'initialisation, attente du registre VIN, puis demande `F6 / request / 10 / [01]`. La MAC est celle de l'interface Wi-Fi cliente, pas un champ ajouté à cette trame. Ne pas confondre ce registre d'écriture avec le registre de télémétrie HVAC `0x10`.

- Une association Wi-Fi demandée, une tentative TCP ; aucune boucle de reconnexion applicative ou relance après reboot. Le pilote Wi-Fi peut effectuer ses propres échanges/réessais radio.
- Attente initialisation/VIN limitée à 20 s ; ACK limité à 10 s ; opération complète limitée à 45 s. Au maximum deux corrections d'encodage `BB` dans la même session, comme la logique existante du protocole ; pas de répétition sur simple timeout.
- Le registre VIN doit être reçu au format MY18/MY2020 de 20 octets, avec moins de deux clients. Autre format/absence : pas d'inscription. Le VIN n'est ni journalisé ni publié.
- Aucun unregister, reset constructeur, réglage de programmation ni commande HVAC. Zigbee reste suspendu ; les autres écritures web, scans et OTA sont bloqués pendant la tentative.
- Après annulation, timeout ou perte de connexion **après envoi**, résultat **incertain** : la voiture peut avoir accepté la demande sans que l'ACK nous parvienne. Ne pas recommencer en boucle ni effacer les clients. Consulter/vérifier l'état avant de décider d'une nouvelle action.
- Même après ACK, le firmware garde l'ancienne MAC en NVS tant que vous ne validez pas explicitement l'adoption de la MAC ESP.

Sans cette fonction, utiliser un poste Wi-Fi et la commande `client register` de la version amont appropriée. Une carte ESP ne peut pas être inscrite par le PC simplement en indiquant sa MAC comme argument : l'identité inscrite est celle de la station effectivement associée à REMOTE.

## Informations à joindre à un rapport de bug

Indiquer année de la voiture, modèle C6, version de firmware, méthode d'inscription, type de MAC (native/privée/clone), disponibilité des places, RSSI approximatif et phase d'échec (Wi-Fi, TCP, initialisation, VIN ou ACK). **Masquer VIN, MAC réelles, suffixe REMOTE, mots de passe, adresses de l'installation et captures personnelles.**
