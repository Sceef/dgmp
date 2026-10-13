// Offscreen rendering of the same loading panel used by the game.
#include "../src/panel.cpp"
#include <vector>
namespace profile {
const std::wstring& Root() { static const std::wstring root=std::filesystem::current_path().wstring(); return root; }
}
namespace devmenu {
bool IsOpen() { return false; }
Snapshot GetSnapshot() { return {}; }
void Click(int,int) {}
}
extern "C" int DGMP_SetDevMenuVisible(int) { return 0; }
int wmain() {
    ULONG_PTR token; Gdiplus::GdiplusStartupInput input;
    if(Gdiplus::GdiplusStartup(&token,&input,nullptr)!=Gdiplus::Ok) return 1;
    const auto path=std::filesystem::path(profile::Root()).parent_path()/L"BendGame"/L"Content"/L"Splash"/L"Splash.bmp";
    panel::background.reset(Gdiplus::Image::FromFile(path.c_str()));
    panel::Loading(true,0.72f,L"Загрузка мира");
    HDC screen=GetDC(nullptr),dc=CreateCompatibleDC(screen);
    HBITMAP bitmap=CreateCompatibleBitmap(screen,1600,900); auto old=SelectObject(dc,bitmap);
    RECT rect{0,0,1600,900}; panel::Paint(dc,rect); SelectObject(dc,old);
    UINT size=0,count=0; Gdiplus::GetImageEncodersSize(&count,&size);
    std::vector<unsigned char> codecs(size); auto encoders=reinterpret_cast<Gdiplus::ImageCodecInfo*>(codecs.data());
    Gdiplus::GetImageEncoders(count,size,encoders); int result=2;
    {
        Gdiplus::Bitmap picture(bitmap,nullptr);
        for(UINT i=0;i<count;++i) if(!wcscmp(encoders[i].MimeType,L"image/png")) {
            result=picture.Save(L"logs/loading-preview.png",&encoders[i].Clsid,nullptr)==Gdiplus::Ok?0:3; break;
        }
    }
    DeleteObject(bitmap); DeleteDC(dc); ReleaseDC(nullptr,screen);
    panel::background.reset(); Gdiplus::GdiplusShutdown(token); return result;
}
