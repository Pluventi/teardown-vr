# Teardown VR (résumé en français)

Teardown VR est un mod de réalité virtuelle non officiel pour **Teardown 2.0.1** (Tuxedo Labs, Steam, PC), qui
passe par OpenXR. Les deux yeux montrent le même instant du jeu (« stéréo synchronisée »). On vise et on tient ses
outils avec les manettes, on se déplace au joystick ou en marchant dans la pièce, et les menus s'affichent sur un
écran virtuel. Le mod a deux parties :

- une DLL que le jeu charge sous le nom `dsound.dll` (OpenXR, Direct3D 12, accroches dans le moteur) ;
- un mod Lua installé parmi les mods intégrés du jeu (caméra, visée, outils, physique).

Il a été fait et testé avec un Meta Quest 3, via Virtual Desktop et Quest Link. SteamVR peut aussi être choisi.

La documentation complète est en anglais : [README.md](README.md).

> **Attention : une mise à jour de Teardown peut casser le mod.**
> Le mod retrouve les fonctions internes du jeu par des motifs d'octets écrits pour la version 2.0.1. Après une
> mise à jour, les fonctions non reconnues se désactivent et l'écrivent dans `TeardownVR.log`. La stéréo
> synchronisée peut repasser en yeux alternés, et le mod peut même ne plus marcher du tout. Aucune correction ni
> mise à jour n'est promise.

## Fonctions principales

- Stéréo synchronisée, avec le champ de vision asymétrique propre au casque.
- Visée à la manette avec un laser. L'outil est tenu dans la main VR, avec ses animations.
- Outil physique, main libre physique, bras visible, masse que l'on balance soi-même.
- Déplacement au joystick, rotation par crans ou fluide, roomscale, vignette de confort.
- Inventaire VR porté à la ceinture, et conduite des véhicules aux manettes.
- Prise en charge des autres mods : visée, réticules, fenêtres et menus en panneaux flottants, caméras, touches
  du clavier.
- Panneau de réglages en anglais ou en français (F10, ou appui long sur Menu de la manette gauche).
- Détection automatique du casque au démarrage.

## Prérequis

- Teardown **2.0.1** (Steam), Windows 10 ou 11 en 64 bits.
- Un casque PC VR avec un runtime OpenXR (testé avec un Quest 3, via Virtual Desktop ou Quest Link).
- Un PC capable de faire tourner Teardown à environ **deux fois** la fréquence du casque (environ 144 i/s pour
  72 Hz).
- Le Microsoft Visual C++ 2015-2022 Redistributable (x64).

## Téléchargement

L'**installateur prêt à l'emploi** est publié sur le Patreon de l'auteur : **https://patreon.com/VRGamesForAll**.
L'installateur peut contenir du bytecode de shaders dérivé de ceux de Teardown (pour les deux fonctions citées
ci-dessous) ; cette partie n'est pas sous GPL et n'est pas dans ce dépôt.

Le code source de ce dépôt produit une version **sans les shaders dérivés du jeu**. Ces shaders sont reconstruits
à partir de ceux de Teardown, donc ils ne sont pas publiés ici. Sans eux :

- la correction des particules en stéréo (pluie, fumée et feu identiques dans les deux yeux) n'est pas
  disponible ;
- les modes d'anticrénelage VR ne sont pas disponibles ;
- le jeu garde ses propres shaders.

### Versions GitHub : compilées par GitHub, vérifiables

La page [Releases](../../releases) propose des versions prêtes à l'emploi de **ce code** (sans les shaders dérivés
du jeu). Elles ne sont compilées sur le PC de personne : le [workflow de publication](.github/workflows/release.yml)
les fabrique sur les machines de GitHub à partir du commit publié, et tout le monde peut le vérifier :

- **SHA-256** : `SHA256SUMS.txt` et les notes de version donnent l'empreinte de chaque fichier. À comparer avec
  `Get-FileHash Teardown-VR.zip -Algorithm SHA256` dans PowerShell.
- **Preuve d'origine** : GitHub signe une attestation qui prouve que les fichiers ont été fabriqués par ce workflow
  à partir de ce dépôt. Vérification : `gh attestation verify Teardown-VR.zip --repo <propriétaire>/<dépôt>`.
- **VirusTotal** : quand les notes de version ont des liens VirusTotal, les fichiers y ont été analysés par des
  dizaines d'antivirus. Tu peux aussi les déposer toi-même sur https://www.virustotal.com.

Le programme n'est pas signé : Windows peut afficher « Windows a protégé votre ordinateur » (« Informations
complémentaires », puis « Exécuter quand même »). Un mod qui se charge dans un jeu sous forme de `dsound.dll` peut
parfois être signalé à tort par un antivirus ; le code est là pour être lu et compilé soi-même.

## Compiler

Il faut Visual Studio 2022 (les Build Tools suffisent) avec le C++ et CMake, ainsi que Git :

```bash
git clone --recursive <adresse du dépôt>
cd <dossier du dépôt>
cmake -S . -B build -A x64
cmake --build build --config Release --target TeardownVR
```

Le résultat est `build/Release/dsound.dll`. Installation à la main :

1. Copier `dsound.dll` à côté de `teardown.exe`.
2. Copier le contenu de `mod/TeardownVR/` dans `<jeu>/mods/teardownvr/`.
3. Activer « Teardown VR » une fois dans le gestionnaire de mods, liste « Intégré ».

Les détails sont dans [docs/BUILDING.md](docs/BUILDING.md), en anglais. Le guide du joueur en français est
[tools/LISEZMOI.txt](tools/LISEZMOI.txt).

## Tickets et contributions

On peut ouvrir des tickets (issues) et des demandes de fusion (pull requests). Rien ne garantit qu'ils seront lus,
traités, corrigés ou acceptés.

Avant de joindre des journaux ou le zip « Préparer mes journaux » à un ticket public, relis-les : les chemins
peuvent contenir ton nom d'utilisateur Windows, et le zip décrit ton PC. Ne publie jamais de fichiers du jeu, de
copies de sa mémoire, de listings désassemblés ni de shaders dérivés de ceux du jeu, ni dans un ticket, ni dans
une demande de fusion.

## Licence

Copyright (C) 2026 VR Games For All.

Teardown VR est un logiciel libre sous licence **GNU GPL, version 3 uniquement** (SPDX : `GPL-3.0-only`, voir
[LICENSE](LICENSE)). Bibliothèques tierces, en sous-modules git dans `third_party/` :

- Dear ImGui (MIT) ;
- MinHook (BSD 2-Clause) ;
- OpenXR SDK (Apache-2.0).

## Avertissement

- Projet de fan non officiel, sans lien avec Tuxedo Labs et sans son approbation.
- Ce dépôt ne contient aucun fichier, code ou shader du jeu.
- Le mod ne modifie, ne retire et ne contourne pas la protection anticopie du jeu.
- Le mod est fourni **tel quel**, sans garantie et **sans assistance**. Aucune réponse, correction, mise à jour
  ou nouvelle fonction n'est promise.

## Crédits

Réalisé par VR Games For All avec l'aide de Claude, l'IA d'Anthropic.
