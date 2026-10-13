@echo off
chcp 65001 > nul
echo Recherche et suppression des dossiers 'obj' en cours...
echo.

:: Boucle récursive pour trouver et supprimer les dossiers 'obj'
for /d /r %%i in (obj) do (
    if exist "%%i" (
        echo Suppression de : "%%i"
        rd /s /q "%%i"
    )
)

echo.
echo Opération terminée !
pause