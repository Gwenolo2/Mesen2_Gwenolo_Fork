#pragma once
#include "pch.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <map>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

// Position d'une tile dans une image d'atlas (en pixels HD)
struct GbHdTileRef
{
	uint32_t ImageIndex = 0; // index dans la liste des <img>
	uint32_t X = 0;
	uint32_t Y = 0;
};

// Condition memoryCheckConstant : (octet CPU & masque) operateur constante(s)
struct GbHdCondition
{
	enum class Op : uint8_t { Eq, Ne, Gt, Lt, Ge, Le, In };
	enum class Kind : uint8_t { Memory, FrameRange, TileAtPosition, SpriteAtPosition, TileNearby, SpriteNearby };
	Kind Type = Kind::Memory;
	uint32_t Divisor = 1;   // frameRange : vrai si (image % Divisor) >= Threshold
	uint32_t Threshold = 0;
	// tileAtPosition / spriteAtPosition
	bool AnyX = false;          // joker * / all / any sur X
	bool AnyY = false;          // joker sur Y
	int32_t X = 0;              // tileAtPosition : colonne en tuiles 8 px ; spriteAtPosition : x en unites de 8 px
	int32_t Y = 0;              // idem pour la ligne
	uint64_t Hash = 0;          // hash 64 bits de la tile cherchee
	uint32_t Palette = 0;       // palette numerique (decimal, comme <tile>)
	bool IgnorePalette = false; // palette « ignore »
	Op Operator = Op::Eq;
	uint8_t Mask = 0xFF;
	std::vector<uint8_t> Values; // une seule valeur, ou la liste de « in »
	uint32_t Slot = 0;           // index de l'adresse dans GbHdPackData::WatchedAddresses
};

// Terme d'une ligne conditionnelle : nom de condition (ou hmirror / vmirror), eventuellement inverse
struct GbHdTerm
{
	std::string Name;
	bool Negated = false;
};

// Tile sous condition : [termes]<tile...>. Ne s'applique que si ses termes sont tous vrais.
struct GbHdConditionalTile
{
	uint8_t Flags = 0; // 2 = hflip, 4 = vflip (comme la cle des tiles)
	std::vector<GbHdTerm> Terms;
	GbHdTileRef Ref;
};

// Operateur de defilement (comme NES / SNES) : scroll combine au ratio
enum class GbHdBgOperator : uint8_t { Multiply, Divide, Plus, Minus };

// Fond <background> (signature SNES) : fichier|dossier,luminosite,scrollX,scrollY,Z,offsetX,offsetY,coucheMaitre,tileX,tileY,framesParImage,inverse,alpha,operateur
struct GbHdBackground
{
	std::vector<uint32_t> Frames; // index d'images : une seule, ou les PNG d'un dossier (ordre alphabetique)
	double Luminosity = 1.0;
	double RatioX = 0.0;
	double RatioY = 0.0;
	int Z = 5;            // niveau : sous les tiles normales (10), sous les sprites (spriteZ)
	int OffsetX = 0;      // pixels SD
	int OffsetY = 0;
	bool TileX = false;   // repetition horizontale
	bool TileY = false;
	uint32_t FramesBetweenSteps = 1; // 0 = premiere image figee ; utile pour un dossier
	bool Reverse = false;
	double Alpha = 1.0;
	GbHdBgOperator Operator = GbHdBgOperator::Multiply;
	std::vector<GbHdTerm> Terms; // vide = toujours vrai
};

// <2DOverlay> : effets 2D calcules, meme JSON que NES / SNES (docs/claude/09_2DOverlay.md).
// Unites Game Boy : pixels SD (160 x 144), secondes, degres (0 = droite, 90 = bas).
struct GbHdParticle
{
	float X = 0, Y = 0;       // position dans le champ (pixels SD)
	float VX = 0, VY = 0;     // vitesse propre
	float Age = 0, Life = 1;  // secondes
	float Size = 1;           // multiplicateur de taille propre (aleatoire)
	float Rot = 0, Spin = 0;  // radians, radians / s
	float Seed1 = 0, Seed2 = 0;
};

struct GbHdOverlay
{
	std::string JsonFile;
	std::vector<GbHdTerm> Terms; // vide = toujours actif

	// Parametres de la balise (defauts, puis JSON, puis balise)
	int ZIndex = 99;                    // echelle Game Boy : fonds 5, tiles 10, sprites 20, tiles prioritaires 30 ; 99 = devant tout
	int Layer = 0;                      // 0 = fixe a l'ecran, 1 = suit le defilement (SCX / SCY)
	float SpeedX = 1.0f, SpeedY = 1.0f; // ratio de defilement (s'ajoute au mouvement propre)
	float Alpha = 1.0f;
	float FieldW = 1.0f, FieldH = 1.0f;

	// Particules (type=particles)
	int TextureIndex = -1;              // image de la particule dans GbHdPackData::Images (pixels HD) ; -1 = point doux
	bool Additive = false;
	int MaxParticles = 300;
	float Rate = 60.0f;
	float LifeMin = 2.0f, LifeMax = 4.0f;
	float SpawnX = 0, SpawnY = 0, SpawnW = 1, SpawnH = 1;
	float SpeedMin = 20, SpeedMax = 40;
	float AngleMin = 80, AngleMax = 100;
	float GravityX = 0, GravityY = 0;
	float WindX = 0, WindY = 0;
	float Turbulence = 0, TurbulenceFreq = 0.5f;
	float RotationMin = 0, RotationMax = 0; // degres
	float SpinMin = 0, SpinMax = 0;         // degres / s
	float SizeStart = 1, SizeEnd = 1, SizeRandom = 0;
	float AlphaStart = 1, AlphaEnd = 1, FadeIn = 0.1f, FadeOut = 0.2f;
	uint32_t ColorStart = 0xFFFFFF, ColorEnd = 0xFFFFFF;
	float DotSize = 2.0f;                   // diametre du point doux (pixels SD)
	bool Prewarm = true;

	// Calques plein ecran : plainscreen, ellipse (zone en fractions de l'image)
	bool IsPlainScreen = false, IsEllipse = false;
	float ZoneX0 = 0, ZoneX1 = 1, ZoneY0 = 0, ZoneY1 = 1;
	uint32_t FxColor = 0x000000;
	float FxEllipseX = 80.0f, FxEllipseY = 72.0f;   // centre (pixels SD)
	float FxRadiusX = 32.0f, FxRadiusY = 16.0f;     // rayons (pixels SD)
	bool FxNegative = false;                        // tout l'ecran couvert sauf l'ellipse

	// Torche (type=torchlight)
	bool IsTorch = false;
	float TorchX = 80.0f, TorchY = 72.0f;    // centre (pixels SD), decale par l'ancrage Layer
	float TorchRadius = 48.0f;
	float TorchSoft = 0.5f;
	bool TorchDark = true;
	float TorchGlow = 0.15f;

	// Ombres, nuages, brouillard (bruit qui derive) : NoiseKind 0 = ombres, 1 = nuages, 2 = brouillard
	bool IsNoise = false;
	int NoiseKind = 0;
	float FxSize = 48.0f;
	float FxSpeed = 8.0f, FxDirection = 0.0f;
	float FxCoverage = 0.5f;
	int FxSeed = 1;

	// Rayons de lumiere (type=rays)
	bool IsRays = false;
	float RayX = 0.5f, RayY = 0.0f;
	int RayCount = 8;
	float RayAngle = 90.0f, RaySpread = 60.0f;
	float RayWidth = 4.0f, RayLength = 0.8f;
	float RaySpeed = 0.2f, RayWobble = 3.0f;
	uint32_t RayColor = 0xFFE8A0;

	// Aurore boreale (type=aurora)
	bool IsAurora = false;
	float AuroraSize = 40.0f, AuroraSpeed = 6.0f, AuroraCoverage = 0.5f, AuroraFall = 0.7f, AuroraWave = 6.0f;
	uint32_t AuroraColor1 = 0x3CFF8C, AuroraColor2 = 0x8C4BFF;

	// Distorsion (type=distortion) : deplacement horizontal sinusoidal des lignes de l'image finale
	bool IsDistortion = false;
	float DistAmp1 = 0, DistWave1 = 32, DistSpeed1 = 0.5f, DistPhase1 = 0;  // px SD, lignes SD, cycles / s, degres
	float DistAmp2 = 0, DistWave2 = 17, DistSpeed2 = 0.3f, DistPhase2 = 0;

	// Etat d'execution (filtre HD)
	std::vector<GbHdParticle> Particles;
	float Time = 0;
	bool Initialized = false;
	bool Active = false;
	float SpawnAcc = 0;
	float OffsetX = 0, OffsetY = 0;
	int LastScrollX = 0, LastScrollY = 0;
	bool HaveScroll = false;
	uint32_t Rng = 0x12345678;

	float Rand() { Rng ^= Rng << 13; Rng ^= Rng >> 17; Rng ^= Rng << 5; return (float)(Rng & 0xFFFFFF) / 16777216.0f; }
	float RandRange(float a, float b) { return a + (b - a) * Rand(); }
};

// <2DSpriteLight> : halo 2D au centre de chaque tile de sprite reconnue (comme SNES / NES).
// [termes]<2DSpriteLight>HASH[,palette|*][,radius=48][,color=FFA040][,intensity=1.4][,flicker=0][,speed=9][,mode=light|add|shadow][,dx=0][,dy=0]
// Unites : pixels SD. dx / dy inverses quand la tile est retournee.
struct GbHdSpriteLight
{
	uint64_t Hash = 0;
	bool AnyPalette = true;
	uint32_t Palette = 0;      // palette encodee D8 (decimal, comme <tile>)
	float Radius = 48.0f;
	float Intensity = 1.4f;
	uint32_t Color = 0xFFA040;
	float Flicker = 0.0f;      // 0 a 1
	float FlickerSpeed = 9.0f; // radians / s
	bool Shadow = false;       // mode=shadow : assombrit
	float OffsetX = 0.0f, OffsetY = 0.0f;
	std::vector<GbHdTerm> Terms;
};

// <muteAudio> : coupe des canaux audio quand les termes sont vrais (propre au Game Boy).
// [termes]<muteAudio>* | Square1,Square2,Wave,Noise (S1, S2 acceptes ; insensible a la casse)
struct GbHdAudioMute
{
	uint8_t Mask = 0; // 1 = Square1, 2 = Square2, 4 = Wave, 8 = Noise
	std::vector<GbHdTerm> Terms;
};

// <bgm>album,piste,fichier.ogg[,positionDeBoucle] : piste OGG (comme NES). Identifiant = album * 256 + piste.
struct GbHdBgmTrack
{
	std::string Filename;      // chemin complet
	uint32_t LoopPosition = 0; // echantillon de reprise de la boucle
};

// [termes]<play_conditional_bgm>album,piste[,boucle] (comme NES) : la premiere regle vraie du fichier choisit la piste.
struct GbHdConditionalBgm
{
	int TrackId = 0;
	bool Loop = true; // boucle = 0 : la piste est jouee une fois
	std::vector<GbHdTerm> Terms;
};

// Contenu d'un pack Game Boy (gb_hires.txt)
struct GbHdPackData
{
	uint32_t Scale = 4;
	std::string FolderPath;
	std::vector<std::string> Images; // chemins relatifs au dossier du pack, dans l'ordre des <img>
	// Cle : (hash de la tile, palette encodee D8, flags : 2 = hflip, 4 = vflip)
	std::map<std::tuple<uint64_t, uint32_t, uint8_t>, GbHdTileRef> Tiles;
	// Conditions declarees par <condition>
	std::map<std::string, GbHdCondition> Conditions;
	// Tiles conditionnelles, dans l'ordre du fichier ; ConditionalIndex : (hash, palette) -> indices
	std::vector<GbHdConditionalTile> ConditionalTiles;
	std::map<std::pair<uint64_t, uint32_t>, std::vector<size_t>> ConditionalIndex;
	// Adresses CPU releves a chaque image (une seule lecture par adresse)
	std::vector<uint16_t> WatchedAddresses;
	// Fonds dans l'ordre du fichier : le premier dont les termes sont vrais est utilise
	std::vector<GbHdBackground> Backgrounds;
	// Niveau des sprites (<spriteZ>, defaut 20) : un fond de niveau inferieur passe derriere les sprites
	int SpriteZ = 20;
	// <disableOriginalTiles> : les tiles de fond SD sans remplacement HD ne sont pas affichees (couleur 0 du fond a la place)
	bool DisableOriginalTiles = false;
	// Effets <2DOverlay>, dans l'ordre du fichier
	std::vector<GbHdOverlay> Overlays;
	// Halos <2DSpriteLight>
	std::vector<GbHdSpriteLight> SpriteLights;
	// Regles <muteAudio>, et masque des canaux coupes : ecrit par le filtre HD a chaque image, lu par l'APU.
	// Partage (shared_ptr) entre le pack du Gameboy et la copie du filtre.
	std::vector<GbHdAudioMute> AudioMutes;
	std::shared_ptr<std::atomic<uint8_t>> AudioMuteMask;
	// Musique OGG : pistes <bgm>, regles <play_conditional_bgm>, et piste demandee pour l'image
	// ((id << 1) | boucle, -1 = aucune), ecrite par le filtre HD, lue par GbHdAudio sur le thread de l'emulation
	std::map<int, GbHdBgmTrack> BgmFiles;
	std::vector<GbHdConditionalBgm> ConditionalBgms;
	std::shared_ptr<std::atomic<int32_t>> BgmRequest;
	bool IsLoaded = false;
};

class GbHdPackLoader
{
public:
	// Lit <packFolder>/gb_hires.txt. Retourne false si le fichier est absent.
	// Journal [HD-GB-LOAD].
	static bool Load(const std::string& packFolder, GbHdPackData& outData);
};
