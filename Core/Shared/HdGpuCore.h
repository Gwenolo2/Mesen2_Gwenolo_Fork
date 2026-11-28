#pragma once
// =====================================================================================
// BASE GPU COMMUNE des packs HD (OpenGL 3.3 core, contexte invisible ouvert par SDL2).
// Voir docs/claude/34_Diorama_Chemin_GPU_NES.md (option C, etape G0).
//
// Independante de la console : ne connait ni le Mode 7 ni la SNES. Premier utilisateur : le diorama NES.
// SnesHdGpu n'est PAS modifie (il garde son propre contexte) ; une fois le diorama valide, il pourra etre
// bascule sur cette classe dans un chantier separe.
//
// G0 : seulement le contexte, une image cachee, un dessin de test et la relecture asynchrone (PBO).
// Indisponible (pas de SDL2.dll, pas d'OpenGL 3.3, coeur libretro...) -> Available() renvoie faux.
// =====================================================================================
#include <vector>
#include <string>
#include <mutex>
#include <thread>
#include <cstdint>

// G3a : vue du diorama (doc 37). Les 4 couches sont 4 plans ; chaque plan est agrandi de (D + Z) / D, donc a vue neutre
// (Yaw = Pitch = 0) tous remplissent exactement l'ecran et l'image est identique a celle du processeur.
// G3b2 : textures des murs (dessus / flancs) d'une couche epaisse, doc 38. Pixels ARGB premultiplies (0xAARRGGBB), l'appelant
// garde la memoire. TopPix : dessus et dessous ; SidePix : flancs gauche et droit ; nullptr = mur d'origine (extrusion).
// TopPerX / TopPerY, SidePerX / SidePerY : pixels NES par repetition du motif, le long du bord (X) et en profondeur (Y).
// Scroll : defilement horizontal (pixels NES) auquel le motif du dessus est ancre.
struct HdDioramaWall
{
	const uint32_t* TopPix = nullptr;
	uint32_t TopW = 0, TopH = 0;
	const uint32_t* SidePix = nullptr;
	uint32_t SideW = 0, SideH = 0;
	float TopPerX = 8.0f, TopPerY = 8.0f;
	float SidePerX = 8.0f, SidePerY = 8.0f;
	float TopOriginX = 0.0f;   // pixels NES : debut du motif du dessus en X (stretchx=adapt : bord gauche de la zone)
	float SideOriginY = 0.0f;  // pixels NES : debut du motif des flancs en Y (stretchx=adapt : bord haut de la zone)
	float Scroll = 0.0f;
	// G3b3 : SideVol > 1 = SidePix est une bande de SideVol images empilees de haut en bas (profondeur), appliquee au bord de chaque
	// tranche en coordonnees du plan ; Vol* : repetition (pixels NES) et origine en X / Y, epaisseur de la couche (pixels NES)
	uint32_t SideVol = 0;
	uint32_t SideVolVert = 0; // 1 = images empilees de haut en bas, 0 = cote a cote
	float VolPerX = 8.0f, VolPerY = 8.0f, VolOriginX = 0.0f, VolOriginY = 0.0f, VolThick = 0.0f;
};

// G4a : complement de l'ecran au-dela des bords d'une couche (doc 40). Cotes : 0 haut, 1 bas, 2 gauche, 3 droite. Margin : pixels NES
// ajoutes au quad (0 = cote sans extension). Mode : 1 stretch, 2 repeat, 3 mirror, 4 wrap. Band : largeur en pixels NES de la bande
// repetee par repeat (tiles x 8).
struct HdDioramaExtend
{
	bool Active = false;
	float Margin[4] = { 0, 0, 0, 0 };
	int Mode[4] = { 0, 0, 0, 0 };
	float Band[4] = { 8, 8, 8, 8 };
	float Range[4][2] = { { -100000.0f, 100000.0f }, { -100000.0f, 100000.0f }, { -100000.0f, 100000.0f }, { -100000.0f, 100000.0f } }; // plage de l'autre axe ou le cote s'applique (cols pour haut / bas, rows pour gauche / droite)
	float Edge[4] = { 0, 240, 0, 256 }; // position (pixels NES) d'ou part l'extension : haut y, bas y, gauche x, droite x
	// G4c : brume. Fog = force atteinte (0..1, 0 = aucune) a FogDist pixels NES du bord ; Color = couleur de la brume (rgb, w = 1 si definie,
	// sinon la brume rend transparent). Mode 5 = aplat de couleur (Color).
	float Fog[4] = { 0, 0, 0, 0 };
	float FogDist[4] = { 128, 128, 128, 128 };
	float Color[4][4] = { { 0, 0, 0, 0 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 } };
};

// G5 : skybox (doc 41) : fond dessine derriere les plans, a la place de la couleur d'effacement. Mode 1 = degrade vertical
// (Top en haut de l'image, Bottom en bas), mode 2 = image Pix (un pixel du PNG = un pixel HD de l'image, repetee) decalee de
// Off (pixels NES).
struct HdDioramaSky
{
	int Mode = 0;
	float Top[3] = { 0, 0, 0 }, Bottom[3] = { 0, 0, 0 };
	const uint32_t* Pix = nullptr;
	uint32_t W = 0, H = 0;
	float OffX = 0.0f, OffY = 0.0f;
	// G5b : modes 3 (panorama equirectangulaire) et 4 (cubemap) : tournent avec la camera de la scene. Rot = decalage de yaw / pitch (degres).
	// CubeLayout : 1 croix 4 x 3, 2 croix 3 x 4, 3 bande 6 x 1, 4 bande 1 x 6 (faces dans l'ordre gauche, devant, droite, derriere, haut, bas).
	float RotYaw = 0.0f, RotPitch = 0.0f;
	float CamPitch = 0.0f; // G5c : inclinaison fixe du ciel (degres) dans le repere de la camera (offset= du pack)
	int CubeLayout = 1;
	float Tint[3] = { 1.0f, 1.0f, 1.0f }; // <DayNight> : teinte du ciel (multiplie les modes 1 a 4)
};

// G6a : lumieres de scene (doc 42). Positions en pixels NES de la scene (x 0..W, y 0..H, z positif = derriere, negatif = devant).
// Type 2 = ponctuelle (Pos = position, Radius = portee), type 3 = soleil (Pos = vecteur unitaire vers la lumiere). Col = couleur x intensite.
struct HdDioramaLight
{
	int Type = 0;
	float Pos[3] = { 0, 0, 0 };
	float Col[3] = { 1, 1, 1 };
	float Radius = 256.0f;
	float Glow = 0.0f;        // halo visible (ajoute de la lumiere a l'ecran autour de la lumiere ponctuelle), 0 = aucun
	float GlowRadius = 48.0f; // rayon du halo (pixels NES a la profondeur de la lumiere)
};

struct HdDioramaLights
{
	bool Active = false;
	float Ambient[3] = { 1.0f, 1.0f, 1.0f }; // sans ambiante : couleurs d'origine, les lumieres ne font qu'ajouter
	HdDioramaLight L[8];
};

// G6c : ombres des sprites (doc 42). Img = masque des silhouettes (meme taille que les couches, alpha > 0 = sprite qui projette) ; seules les
// lignes HD Y0 .. Y0 + Rows - 1 sont envoyees. L'ombre tombe sur les couches de OntoMask (bits 0..3 = L0..L3, 4..7 = groupes) : pour chaque pixel
// recepteur, le rayon vers la lumiere numero Light coupe le plan des sprites (L3) ; si le masque y est plein, le pixel est assombri.
struct HdDioramaShadow
{
	bool Active = false;
	const uint32_t* Img = nullptr;
	uint32_t Y0 = 0, Rows = 0;
	float Opacity = 0.5f, Soft = 0.0f;
	float Color[3] = { 0, 0, 0 };
	uint32_t OntoMask = 4; // L2
	int Light = 0;         // numero (0 = premiere) parmi les lumieres ponctuelles / solaires actives
};

// Nombre de groupes <DioramaDepth> (couches supplementaires au-dela de L0..L3) pris en charge par la scene.
static constexpr int HdDioMaxGroups = 8;

// G6d : cartes de normales (doc 44). Img[i] : i = 0 L2, 1 L3, 2 a 5 groupes 1 a 4 ; meme taille que les couches (RGB = normale, alpha 255 = definie).
// L2 et L3 : seules les lignes HD Y0[i] .. Y0[i] + Rows[i] - 1 sont envoyees ; groupes : la bande du groupe (ExtraY0 / ExtraRows).
struct HdDioramaNormals
{
	bool Active = false;
	const uint32_t* Img[2 + HdDioMaxGroups] = {};
	uint32_t Y0[2 + HdDioMaxGroups] = {}, Rows[2 + HdDioMaxGroups] = {};
	float Strength = 1.0f;
	bool FlipY = false;
};

// H1 : objets 3D de decor (doc 45). Position en pixels NES de la scene (comme les lumieres) ; Rot = rotation du modele (3 lignes).
struct HdDioramaObject
{
	const float* Verts = nullptr;   // 8 flottants par sommet : position, normale, uv ; triangles deplies
	uint32_t VertCount = 0;
	uint32_t Version = 0;           // change quand les sommets ont change (modele anime) : renvoi au GPU
	const uint32_t* TexPix = nullptr; uint32_t TexW = 0, TexH = 0;
	float Color[3] = { 1, 1, 1 };
	float Pos[3] = { 0, 0, 0 };
	bool Lit = true;                                // eclaire par les lumieres de la scene (H2)
	float Bright = 1.0f;                            // bright= : multiplie la couleur finale
	float RefZ = 0;                                 // profondeur du modele (derriere sa face avant) a l'echelle de la vue 2D
	bool Occlude = false;                           // dessine en premier ; les tranches suivantes sont testees contre sa profondeur (sprites coupes par la geometrie)
	bool Shadow = true;                             // H3 : recoit les ombres des sprites (G6c)
	bool CastShadow = false;                        // topDown : projette son ombre sur le sol (castshadow=on)
	bool WinOn = false; float WinX0 = 0, WinX1 = 0; // fenetre en x (unites du modele, avant rotation et echelle) : triangles dessines seulement dedans
	float Scale = 16.0f;
	float Rot[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
};

const int HdDioMaxObjects = 256; // Tile2Obj (doc 53) : nombre d'objets 3D actifs par image (etait 8)

// <3DParticles> (diorama NES) : un lot de particules, 12 flottants chacune (position scene centree x, y, z, taille ; r, g, b, alpha ;
// trainee x, y, z (stretch=, vecteur tete -> queue inverse), 0), tableau complete par lignes de 256 particules (texture RGBA32F de 768 texels de large).
struct HdDioramaP3Batch
{
	const float* Data = nullptr; uint32_t Count = 0;
	const uint32_t* TexPix = nullptr; uint32_t TexW = 0, TexH = 0;
	bool Additive = false; float MinPx = 1.5f;
};

struct HdDioramaObjects
{
	int Count = 0;
	HdDioramaObject O[HdDioMaxObjects];
};

// Fond <background> dessine par le GPU a la place de L0 (doc 52) : texture du PNG repetee sur un grand plan a la profondeur de L0
// <2DTexture> sur un fond du diorama : les pixels de la couleur Key (ou transparents si HasKey = false) sont remplaces par la texture
// repetee. Texel = (pixel HD de l'ecran + Off) x Texel (memes regles que le filtre normal).
struct HdDioramaColorTex
{
	const uint32_t* Pix = nullptr; uint32_t W = 0, H = 0;
	bool HasKey = true; uint32_t Key = 0; // RRGGBB
	float Tol = 0.0f;                     // tolerance= : ecart maximal par canal (0 a 255)
	float TexelX = 1.0f, TexelY = 1.0f;   // texels par pixel HD
	float OffX = 0.0f, OffY = 0.0f;       // decalage (pixels HD) : scroll PPU x ratio x echelle
	const uint32_t* MatPix = nullptr; uint32_t MatW = 0, MatH = 0; // materiau : R normale x, G normale y, B AO, A rugosite (nullptr = aucun)
	float NStrength = 1.0f, AoStrength = 1.0f, Specular = 0.5f;
	float Flow[4] = { 0, 0, 0, 0 };   // decalage de la normale (fraction de la texture) : couche 1 (x, y), couche 2 (z, w)
	bool HasFlow2 = false; float Flow2Scale = 1.37f;
};

struct HdDioramaBgPlane
{
	const uint32_t* Pix = nullptr; uint32_t W = 0, H = 0;   // image du fond (premultipliee), pixels HD
	float OffX = 0, OffY = 0;                               // (left + defilement) x echelle, (top + defilement) x echelle
	float Bright = 1.0f;                                    // luminosite 0..1
	bool TileX = false, TileY = false;
	bool Flat = false; float InclC = 1.0f, InclS = 0.0f; // doc 53 : fond a plat (topDown) et cos / sin de son inclinaison
	HdDioramaColorTex Ct[4]; int CtN = 0;                  // <2DTexture> : remplacements de couleur
	bool HasZ = false; float Z = 0.0f;                     // fond d'un groupe <DioramaDepth> : plan a la profondeur du groupe (sinon devant L0)
};

struct HdDioramaScene
{
	HdDioramaBgPlane Bg[8]; int BgCount = 0;  // fonds dessines par le GPU (L0 et groupes <DioramaDepth> sans epaisseur), du plus loin au plus pres
	float BgYStart = 0;                       // premiere ligne NES de l'image des couches (position absolue du fond)
	float BgMargin = 1024.0f;                 // demi-marge du plan (pixels NES) autour de l'ecran
	HdDioramaSky Sky;
	HdDioramaLights Lights;               // G6a : lumieres de scene
	HdDioramaShadow Shadow;               // G6c : ombres des sprites
	HdDioramaNormals Normals;             // G6d : cartes de normales
	HdDioramaObjects Objects;             // H1 : objets 3D de decor
	HdDioramaP3Batch P3[4]; int P3Count = 0; // <3DParticles> (diorama NES)
	float P3Tint[3] = { 1.0f, 1.0f, 1.0f };   // <DayNight> : teinte des particules 3D non additives
	// <DioramaFog> (doc 54) : brouillard de distance (profondeur au-dela du plan de l'ecran, pixels NES) et brume au sol (hauteur)
	// <DioramaClouds> (doc 54) : [0] ombres de nuages, [1] taches de brouillard ; Off = position monde du point de l'ecran (0, 0) + derive (pixels NES)
	struct Cloud { bool Active = false; float Size = 64, Coverage = 0.5f, Soft = 0.15f, Opacity = 0.3f, OffX = 0, OffY = 0, Seed = 1, GroundH = 16; float Col[3] = { 0, 0, 0 }; } Cloud[2];
	// <DioramaMist> (doc 54) : nappes de brume (plans de From a To ; Off = position monde du point (0, 0) de la scene + derive, par nappe)
	struct Mist { bool Active = false; int Count = 0; float From = 0, To = 0, Opacity = 0, Size = 64, Coverage = 0.5f, Soft = 0.3f, Seed = 3, Band = 64; float Col[3] = { 1, 1, 1 }; float OffX[8] = {}, OffY[8] = {}; } Mist;
	struct Fog { bool Active = false; float Start = 0, End = 1, Opacity = 0, Ground = 0, GroundH = 16; float Col[3] = { 0.7f, 0.75f, 0.8f }; } Fog;
	float W = 256, H = 240;     // taille de la scene (pixels NES)
	float OutPad = 0.0f;        // widescreen : colonnes NES ajoutees de chaque cote de l'image de sortie (0 = image de la taille des couches)
	float D = 256;              // distance de la camera au plan Z = 0 (la focale vaut D)
	float Yaw = 0, Pitch = 0;   // degres ; Yaw > 0 rapproche le cote droit, Pitch > 0 eloigne le bas
	float PlaneZ[4] = { 200, 24, 0, -24 }; // profondeur de L0..L3 (positif = derriere)
	float Thick[4] = { 0, 0, 0, 0 };       // G3b1 : epaisseur de L0..L3 (pixels NES), de PlaneZ (face avant) a PlaneZ + Thick. 0 = feuille
	float Scale = 1;            // pixels HD par pixel NES
	uint32_t Clear = 0xFF000000; // couleur hors des plans (ARGB opaque)
	int Aa = 1;                  // G3f : surechantillonnage 1 a 4 (scene dessinee a Aa fois la taille, puis moyenne par blocs Aa x Aa)
	uint32_t FlatMask = 0;      // doc 53 (topDown) : couches posees a plat (bit n = couche n : 0..3 = L0..L3, 4..11 = groupes 1..8)
	// G3c : couche fixe "off camera" (doc 38), dessinee a plat apres la scene. Fixed = image complete (meme taille que les
	// couches) ; seules les lignes HD FixedY0 .. FixedY0 + FixedRows - 1 sont envoyees et dessinees. nullptr = aucune.
	const uint32_t* Fixed = nullptr;
	uint32_t FixedY0 = 0, FixedRows = 0;
	// G3c3 (fixed3d) : la couche fixe est une plaque de FixedThick pixels NES d'epaisseur, face avant a la profondeur FixedZ (taille
	// apparente inchangee), tranches arriere a FixedZ + profondeur x FixedPersp. FixedThick = 0 : a plat comme avant.
	float FixedThick = 0.0f, FixedZ = 0.0f, FixedPersp = 1.0f;
	float FixedDark = 0.0f; // assombrissement des tranches vers le fond : couleur x (1 - FixedDark x profondeur / epaisseur)
	// G3e : couches supplementaires <DioramaDepth> (doc 38). ExtraImg[g] = image complete (meme taille que les couches) ;
	// seules les lignes HD ExtraY0 .. ExtraY0 + ExtraRows - 1 sont envoyees et dessinees. nullptr ou 0 ligne = groupe absent.
	const uint32_t* ExtraImg[HdDioMaxGroups] = {};
	// topDown : pied des sprites, 256 octets par ligne du plan (ligne du plan du bas de la silhouette + 1 ; 0 = pas de sprite) ; nullptr = aucun
	const uint8_t* SpriteFoot = nullptr; uint32_t SpriteFootH = 0;
	// topDown : silhouettes de sprites (16 au plus) : x debut, x fin (exclue), ligne du haut, pied (lignes du plan) ; pour les ombres
	float SpriteBlob[16][4] = {}; int SpriteBlobN = 0;
	int SpriteBlobTotal = 0; // nombre total de silhouettes (au-dela de 16 : sprites sur le plan entier, comme avant)
	// Liste etendue (SNES, doc 59) : jusqu'a 64 silhouettes de sprites dessinees debout (SpriteBlob[16] reste pour les ombres) ;
	// SpriteBlobXN = 0 : non fournie (NES, Game Boy : limite de 16 comme avant)
	float SpriteBlobX[64][4] = {}; int SpriteBlobXN = 0;
	const uint32_t* ExtraMask[HdDioMaxGroups] = {}; // topDown : tiles de montagne du groupe (alpha 255), pour la profondeur seule
	uint32_t ExtraY0[HdDioMaxGroups] = {}, ExtraRows[HdDioMaxGroups] = {};
	float ExtraZ[HdDioMaxGroups] = {};      // profondeur de la face avant (pixels NES)
	float ExtraThick[HdDioMaxGroups] = {};  // epaisseur (pixels NES)
	// topDown, stand=on (doc 59) : le groupe n'est pas pose a plat ; chaque silhouette (x debut, x fin exclue, ligne du haut, pied, en lignes
	// du plan) est dessinee debout a ses pieds comme un sprite. La carte SpriteFoot doit contenir aussi les pieds de ces silhouettes.
	// Le bit du groupe doit etre retire de FlatMask par l'appelant. ExtraStandN = 0 : groupe dessine normalement.
	bool ExtraStand[HdDioMaxGroups] = {};
	float ExtraBlob[HdDioMaxGroups][64][4] = {};
	int ExtraBlobN[HdDioMaxGroups] = {};
	// G3h : envoi reduit. L0Uniform : L0 est une seule couleur (envoyee en 1 x 1). RowDirty[k] (k = 0..3, nullptr = image entiere) :
	// 240 marques par ligne NES, seules les lignes marquees de la couche k sont renvoyees (les autres n'ont pas change depuis
	// le dernier envoi). Ligne HD = (ligne NES - RowY0) * RowScale.
	bool L0Uniform = false;
	uint32_t L0WideW = 0, L0WideH = 0; float L0WideMX = 0, L0WideMY = 0; // taille (pixels HD) de L0Wide et marges (pixels NES) de chaque cote
	const uint32_t* L0Wide = nullptr; // widescreen : image de L0 elargie (largeur = couches + 2 x OutPad x Scale, meme hauteur), bandes comprises ; nullptr = L0 normale
	const uint8_t* RowDirty[4] = { nullptr, nullptr, nullptr, nullptr };
	uint32_t RowY0 = 0, RowScale = 0;
	HdDioramaExtend Ext[4 + HdDioMaxGroups];                // G4a : extensions des couches L0..L3 puis des groupes 0 a 3 (gauche et droite seulement)
	float Zoom = 1.0f;                     // G4 : zoom de la vue (1 = normal)
	bool ExtDebug = false;                 // G4 : teinte magenta des pixels venus d'une extension
	float ExtScrollX = 0.0f, ExtScrollY = 0.0f; // defilement (pixels) auquel les bandes repeat sont alignees
	HdDioramaWall Wall[4 + HdDioMaxGroups];                 // G3b2 : murs des couches 0 a 3 (L0..L3) puis des groupes 0 a 3
};

// Temps cumules (ms) depuis le dernier TakeLayerTimes, pour SubmitLayers / Fetch (mesure du cout, G2b).
struct HdGpuTimes
{
	double UploadMs = 0;  // envoi des 4 textures (glTexImage2D)
	double DrawMs = 0;    // dessin + lancement de la relecture (retour immediat)
	double MapMs = 0;     // attente de la carte graphique (MapBufferRange)
	double FlipMs = 0;    // retournement des lignes dans le tampon de sortie
	uint32_t Count = 0;   // nombre de SubmitLayers
};

class HdGpuCore
{
public:
	static HdGpuCore& Get();
	// Diorama SNES (doc 59) : relache le contexte, pour qu'un autre contexte OpenGL du meme thread (SnesHdGpu, Mode 7) ne soit pas pris pour le sien
	void ReleaseForOtherContext() { ReleaseCurrent(); }

	// Ouvre le contexte au premier appel (une seule tentative). Faux = pas de GPU.
	bool Available();
	const std::string& Info() const { return _info; }

	// G0 : dessine dans une image w x h un triangle colore anime (selon frame) et un carre jaune translucide,
	// sur fond transparent, puis lance la relecture asynchrone. Retour immediat apres l'envoi.
	bool SubmitTest(uint32_t w, uint32_t h, uint32_t frame);
	// G2b : compose 4 images de couches (ARGB premultiplie, w x h, lignes de haut en bas) : l0 opaque, puis l1, l2, l3
	// par-dessus, dans un seul passage de fragments, et lance la relecture. Pixel pour pixel (aucun filtrage).
	bool SubmitLayers(const uint32_t* l0, const uint32_t* l1, const uint32_t* l2, const uint32_t* l3, uint32_t w, uint32_t h);
	// Recupere l'image dessinee par SubmitTest ou SubmitLayers : ARGB premultiplie (0 = rien), lignes de haut en bas.
	bool Fetch(std::vector<uint32_t>& outColor);
	// G3h : comme Fetch, mais ecrit directement count pixels opaques (alpha force a FF) dans dst : une seule passe. Faux si la
	// taille de l'image en attente differe de count (dst n'est alors pas touche).
	bool FetchOpaque(uint32_t* dst, size_t count);
	// G3a : dessine les 4 couches comme 4 plans textures, tries du plus loin au plus pres, vus par une camera en orbite.
	bool SubmitScene(const uint32_t* l0, const uint32_t* l1, const uint32_t* l2, const uint32_t* l3, uint32_t w, uint32_t h, const HdDioramaScene& scene);
	// Renvoie les temps cumules et les remet a zero.
	HdGpuTimes TakeLayerTimes();

private:
	HdGpuCore() = default;
	bool Init();
	bool MakeCurrent();
	void ReleaseCurrent();
	bool EnsureTarget(uint32_t w, uint32_t h);
	bool BuildProgram();
	bool BuildLayerProgram();
	bool BuildSceneProgram();
	void StartReadback(uint32_t w, uint32_t h);

	std::mutex _lock;
	bool _tried = false, _ok = false;
	std::string _info;
	void* _window = nullptr;
	void* _context = nullptr;
	std::thread::id _currentThread; // thread ou le contexte est actif (evite un changement de contexte par frame)

	unsigned int _prog = 0, _vao = 0;
	unsigned int _progLayers = 0;
	bool _layerProgFailed = false;
	unsigned int _progScene = 0;
	bool _sceneProgFailed = false;
	unsigned int _layerTex[4] = { 0, 0, 0, 0 };
	unsigned int _l0WideTex = 0; // widescreen : L0 elargie (fonds <background> dans les bandes)
	bool _layerStore[4] = { false, false, false, false }; // G3h : la texture contient deja une image complete w x h (envoi partiel permis)
	unsigned int _fixedTex = 0; // G3c : bande de la couche fixe
	// G3f : surechantillonnage : image intermediaire Aa fois plus grande, puis passe de moyenne vers l'image finale
	unsigned int _aaFbo = 0, _aaTex = 0;
	uint32_t _aaW = 0, _aaH = 0;
	unsigned int _progResolve = 0;
	unsigned int _progSky = 0;
	unsigned int _shadowTex = 0;
	unsigned int _nrmTex[2 + HdDioMaxGroups] = {};
	bool _skyProgFailed = false;
	bool BuildSkyProgram();
	// H1 : objets 3D : programme de maillage, donnees des sommets en texture (3 texels par sommet), profondeur propre a la passe
	unsigned int _progObj = 0;
	bool _objProgFailed = false;
	bool BuildObjProgram();
	unsigned int _progP3 = 0, _p3DataTex = 0; bool _p3ProgFailed = false; // <3DParticles>
	bool BuildP3Program();
	struct ObjMesh { const float* Key; uint32_t N; unsigned int Tex; uint32_t Version; };
	std::vector<ObjMesh> _objMeshes;
	unsigned int _objDepthTex = 0;
	uint32_t _objDepthW = 0, _objDepthH = 0;
	bool _resolveProgFailed = false;
	bool EnsureAaTarget(uint32_t w, uint32_t h);
	bool BuildResolveProgram();
	unsigned int _extraTex[HdDioMaxGroups] = {}; // G3e : bandes des couches <DioramaDepth>
	unsigned int _sprFootTex = 0; // topDown : pied des sprites (R8)
	unsigned int _extraMaskTex[HdDioMaxGroups] = {}; // masque de profondeur des groupes (topDown)
	// G3b2 : textures de murs envoyees une fois (cle = adresse des pixels, taille et signature de quelques pixels)
	struct WallTex { const uint32_t* Pix; uint32_t W, H, Sig; unsigned int Tex; };
	std::vector<WallTex> _wallTex;
	struct BgTex { const uint32_t* Pix; uint32_t W, H, Sig; unsigned int Tex; };
	std::vector<BgTex> _bgTex;
	unsigned int BgTexture(const uint32_t* pix, uint32_t w, uint32_t h, bool tileX, bool tileY);
	unsigned int WallTexture(const uint32_t* pix, uint32_t w, uint32_t h);
	uint32_t _layerW = 0, _layerH = 0;
	HdGpuTimes _times;
	unsigned int _fbo = 0, _tex = 0;
	unsigned int _pbo = 0;
	size_t _pboCapacity = 0;
	uint32_t _w = 0, _h = 0;
	bool _pending = false;
	int _pendW = 0, _pendH = 0;
};
