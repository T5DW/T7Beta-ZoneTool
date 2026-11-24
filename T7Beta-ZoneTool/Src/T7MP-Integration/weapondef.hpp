#pragma once
#include <string>
namespace WeaponDef
{
    void Setup();
    void CatalogFrame();
    void Request();
    void RequestDump(const std::string&);
    void ServerFrame(bool active);
    void DrawError();
}
