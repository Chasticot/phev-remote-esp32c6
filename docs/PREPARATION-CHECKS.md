# Contrôles de préparation publique — 2026-10-08

Version `0.1.0-beta.1`. Aucune commande voiture ni mise à jour de la carte en service durant cette préparation.

- Compilation PlatformIO avec plateforme officielle figée `55.03.312` : succès.
- Tests Python firmware/packaging : 22 tests, dont absence de MAC prédéfinie, conservation NVS, saisie AP, tableau de bord et suppression des chemins compilateur.
- Tests templates HA : 32/32, y compris valeurs numériques natives, dépendances PyYAML 6.0.3/Jinja2 3.1.6 installées isolément ; aucun test ignoré.
- 8 suites C++ hôte : succès, protocole 31/31 dans chacune des deux variantes, maintenance, ZCL, sessions/cache, résultat atomique, compatibilité TCP et validation MAC.
- Convertisseur JavaScript : succès, traitement groupé ordonné et 28 liaisons.
- Contrôle ciblé des identifiants privés connus dans les fichiers publiables et le binaire : aucun trouvé après retrait des chemins compilateur. Aucun journal ni capture personnelle inclus. Ce contrôle ciblé n'est pas un audit de sécurité exhaustif.

Image d'application locale : SHA256 `7FA5DDB84EC21F1D45D74CFA5F1781A3996E7F61F7C5013AE5D6C33C3FE74A9F`. Un build sur une autre machine peut produire un hash différent (voir dépendances).

Workflow GitHub Actions fourni mais son exécution distante doit être vérifiée après publication ; les résultats ci-dessus sont locaux. Les tests hôte ont été exécutés avec Zig 0.13.0 sous Windows ; la CI utilise g++ sous Ubuntu. Ne pas assimiler cela à un nouveau test physique du véhicule.
