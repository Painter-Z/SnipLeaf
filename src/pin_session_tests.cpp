#include "app.h"
#include <bit>
#include <wincrypt.h>
namespace snap
{
std::string json(std::wstring const &);
void writeReport(std::wstring const &, std::string const &);
void require(bool, char const *);
namespace
{
struct Suite
{
    int passed{}, failed{};
    std::string cases;
    void test(wchar_t const *name, std::function<void()> body)
    {
        if (!cases.empty()) cases += ',';
        cases += "{\"name\":" + json(name);
        try { body(); ++passed; cases += ",\"passed\":true}"; }
        catch (...) { ++failed; cases += ",\"passed\":false,\"error\":" + json(errorText()) + '}'; }
    }
    int report(std::wstring const &path, char const *name)
    {
        writeReport(path, "{\"suite\":\"" + std::string(name) + "\",\"passed\":" + std::to_string(passed) +
                          ",\"failed\":" + std::to_string(failed) + ",\"cases\":[" + cases + "]}");
        return failed ? 1 : 0;
    }
};
SavedPin examplePin(int x = 100, int y = 120)
{
    Image image(180, 110);
    for (size_t i = 0; i < image.pixels.size(); i += 4)
    {
        image.pixels[i] = static_cast<uint8_t>(i % 241);
        image.pixels[i+1] = static_cast<uint8_t>((i / 4) % 253);
    }
    SavedPin pin;
    pin.base = std::make_shared<Image>(std::move(image));
    pin.placement = {{x+1,y+1}, 1.25f};
    pin.window = {x,y,x+227,y+140};
    for (auto kind : {Tool::Arrow, Tool::Line, Tool::Text, Tool::Mosaic, Tool::Marker, Tool::Rectangle, Tool::Ellipse})
    {
        Annotation mark{kind, {20,25}, {150,80}, {}, L"", 0x006abc, 2.5f, 17.5f, 9};
        if (kind == Tool::Text) mark.text = L"中英混合 SnipLeaf\r\nSecond paragraph 第二段";
        if (kind == Tool::Marker) mark.path = {{20,25},{80,50},{150,80}};
        pin.marks.push_back(std::move(mark));
    }
    return pin;
}
bool sameMark(Annotation const &a, Annotation const &b)
{
    if (a.kind != b.kind || a.from.x != b.from.x || a.from.y != b.from.y || a.to.x != b.to.x ||
        a.to.y != b.to.y || a.text != b.text || a.width != b.width || a.fontPt != b.fontPt ||
        a.rgb != b.rgb || a.mosaicPixels != b.mosaicPixels || a.path.size() != b.path.size()) return false;
    for (size_t i = 0; i < a.path.size(); ++i)
        if (a.path[i].x != b.path[i].x || a.path[i].y != b.path[i].y) return false;
    return true;
}
void samePin(SavedPin const &a, SavedPin const &b)
{
    require(a.base && b.base && a.base->width == b.base->width && a.base->height == b.base->height &&
            a.base->pixels == b.base->pixels, "Base pixels changed across restart.");
    require(a.marks.size() == b.marks.size(), "Annotation count changed.");
    for (size_t i = 0; i < a.marks.size(); ++i)
        require(sameMark(a.marks[i], b.marks[i]), "Editable annotation changed.");
    require(EqualRect(&a.window, &b.window) && a.placement.imageOrigin.x == b.placement.imageOrigin.x &&
            a.placement.imageOrigin.y == b.placement.imageOrigin.y && a.placement.zoom == b.placement.zoom &&
            a.viewOrigin.x == b.viewOrigin.x && a.viewOrigin.y == b.viewOrigin.y &&
            a.visible == b.visible && a.smoothZoom == b.smoothZoom, "Pin geometry or display mode changed.");
}
std::vector<uint8_t> readFile(std::filesystem::path const &file)
{
    std::ifstream input(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
void putFile(std::filesystem::path const &file, std::span<const uint8_t> bytes)
{
    std::filesystem::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary);
    out.write(reinterpret_cast<char const *>(bytes.data()), bytes.size());
    require(out.good(), "Cannot write synthetic test fixture.");
}
// Corrupt authenticated *synthetic* metadata, then re-protect it so the parser,
// rather than just DPAPI integrity checking, receives malformed input.
void patchPlain(std::filesystem::path const &file, size_t offset, uint32_t value, bool truncate = false)
{
    auto bytes = readFile(file);
    DATA_BLOB source{static_cast<DWORD>(bytes.size()), bytes.data()}, plain{}, encrypted{};
    require(CryptUnprotectData(&source,nullptr,nullptr,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&plain) != FALSE,
            "Cannot decrypt synthetic session.");
    require(offset+4 <= plain.cbData, "Invalid test metadata offset.");
    std::memcpy(plain.pbData+offset, &value, 4);
    if (truncate) plain.cbData = static_cast<DWORD>(offset+4);
    bool protectedOk = CryptProtectData(&plain,L"test",nullptr,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&encrypted) != FALSE;
    LocalFree(plain.pbData);
    require(protectedOk, "Cannot protect synthetic malformed session.");
    putFile(file,{encrypted.pbData,encrypted.cbData});
    LocalFree(encrypted.pbData);
}
bool throws(std::function<void()> body)
{
    try { body(); return false; } catch (...) { return true; }
}
} // namespace
int App::pinSessionTests(std::wstring const &report)
{
    Suite suite;
    // A new directory per run; neither credentials nor real pin caches are touched.
    GUID guid{}; check(CoCreateGuid(&guid)); wchar_t id[40]{}; StringFromGUID2(guid,id,40);
    auto root = std::filesystem::path(report).parent_path() / L"pin-session-fixtures" / id;
    auto snapshot = PinSnapshot{{examplePin(-470,140),examplePin(420,-240)},false};
    snapshot.pins[1].visible = false; snapshot.pins[1].smoothZoom = false;
    auto save = [&](std::wstring const &name)
    {
        PinSessionStore store(root/name); store.save(graphics,snapshot); return store;
    };
    suite.test(L"首次启动没有缓存时为空会话", [&]
    { require(PinSessionStore(root/L"missing").load(graphics).pins.empty(), "Missing cache was not empty."); });
    suite.test(L"多图像素、全部标注、中英文和负坐标加密往返", [&]
    {
        auto store = save(L"roundtrip"); auto restored = store.load(graphics);
        require(restored.pins.size() == 2 && !restored.pinsVisible, "Pin count or global visibility changed.");
        for (size_t i=0;i<2;++i) samePin(snapshot.pins[i],restored.pins[i]);
    });
    suite.test(L"缓存中没有明文 PNG、标注文字或临时文件", [&]
    {
        auto store=save(L"encrypted"); auto bytes=readFile(store.file());
        auto png=pngBytes(graphics,*snapshot.pins[0].base);
        require(std::search(bytes.begin(),bytes.end(),png.begin(),png.begin()+8)==bytes.end(),"Plain PNG leaked.");
        auto text=snapshot.pins[0].marks[2].text;
        auto first=reinterpret_cast<uint8_t const *>(text.data());
        require(std::search(bytes.begin(),bytes.end(),first,first+text.size()*2)==bytes.end(),"Plain text leaked.");
        size_t count{}; for(auto const &entry:std::filesystem::directory_iterator(store.file().parent_path()))
            if(entry.is_regular_file()) ++count;
        require(count==1,"Temporary or unencrypted files left behind.");
    });
    suite.test(L"空会话覆盖旧会话后不复活已关闭贴图", [&]
    { auto store=save(L"empty"); store.save(graphics,{}); require(store.load(graphics).pins.empty(),"Closed pin resurrected."); });
    suite.test(L"原子替换失败保留原会话", [&]
    {
        auto store=save(L"locked"); auto original=readFile(store.file());
        HANDLE lock=CreateFile(store.file().c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
        require(lock!=INVALID_HANDLE_VALUE,"Cannot lock synthetic session.");
        bool rejected=throws([&]{store.save(graphics,{});}); CloseHandle(lock);
        require(rejected && readFile(store.file())==original,"Failed save destroyed last good state.");
        require(store.load(graphics).pins.size()==2,"Last good state no longer readable.");
    });
    suite.test(L"损坏密文被拒绝且读取不覆盖原文件", [&]
    {
        auto store=save(L"corrupt"); auto bytes=readFile(store.file()); bytes.back()^=0x55; putFile(store.file(),bytes);
        require(throws([&]{store.load(graphics);}) && readFile(store.file())==bytes,"Corrupt file accepted or overwritten.");
    });
    suite.test(L"未知格式版本被拒绝", [&]
    { auto store=save(L"schema"); patchPlain(store.file(),4,99); require(throws([&]{store.load(graphics);}),"Unknown schema accepted."); });
    suite.test(L"伪造贴图数量被拒绝", [&]
    { auto store=save(L"count"); patchPlain(store.file(),12,0xffffffff); require(throws([&]{store.load(graphics);}),"Oversized count accepted."); });
    suite.test(L"NaN 缩放被拒绝", [&]
    { auto store=save(L"nan"); patchPlain(store.file(),28,0x7fc00000); require(throws([&]{store.load(graphics);}),"NaN zoom accepted."); });
    suite.test(L"截断的认证数据被拒绝", [&]
    { auto store=save(L"truncated"); patchPlain(store.file(),16,3,true); require(throws([&]{store.load(graphics);}),"Truncated payload accepted."); });
    suite.test(L"无效标注类型被拒绝", [&]
    { auto store=save(L"tool"); patchPlain(store.file(),60,0xffffffff); require(throws([&]{store.load(graphics);}),"Invalid annotation accepted."); });
    suite.test(L"保存时拒绝无穷坐标而不破坏旧缓存", [&]
    {
        auto store=save(L"invalid-save"); auto before=readFile(store.file()); auto invalid=snapshot;
        invalid.pins[0].marks[0].from.x=std::numeric_limits<float>::infinity();
        require(throws([&]{store.save(graphics,invalid);}) && readFile(store.file())==before,"Invalid save destroyed cache.");
    });
    suite.test(L"PNG 解码前执行像素预算检查", [&]
    {
        auto png=pngBytes(graphics,*snapshot.pins[0].base);
        require(throws([&]{imageFromPng(graphics,png,100);}),"Decoded over-budget image.");
    });
    suite.test(L"负坐标显示器上的位置完全保留", [&]
    {
        std::array<RECT,2> monitors{{{-1920,0,0,1080},{0,0,1920,1080}}};
        auto pin=examplePin(-1000,300); auto restored=reachablePin(pin,monitors); samePin(pin,restored);
    });
    suite.test(L"部分离屏但可拖动的贴图不自动居中", [&]
    {
        std::array<RECT,1> monitors{{{0,0,1920,1080}}};
        auto pin=examplePin(-150,250); samePin(pin,reachablePin(pin,monitors));
    });
    suite.test(L"显示器移除后将不可见贴图找回", [&]
    {
        std::array<RECT,1> monitors{{{0,0,1920,1080}}}; auto pin=examplePin(-5000,5000);
        auto restored=reachablePin(pin,monitors); RECT overlap{};
        require(IntersectRect(&overlap,&restored.window,&monitors[0]) && overlap.right-overlap.left>24 &&
                overlap.bottom-overlap.top>24 && restored.placement.zoom==pin.placement.zoom,"Unreachable pin not recovered.");
    });
    suite.test(L"移动贴图后后台保存和 flush 保留最新代次", [&]
    {
        auto dir=root/L"worker"; PinSession session(dir); uint64_t last{};
        for(int i=0;i<30;++i) { auto state=snapshot; state.pins[0]=examplePin(50+i,60+i); last=session.submit(std::move(state)); }
        std::wstring error; require(session.flush(last,error),"Worker save failed.");
        auto restored=PinSessionStore(dir).load(graphics);
        require(restored.pins[0].window.left==79 && restored.pins[0].window.top==89,"Old pending snapshot won.");
    });
    suite.test(L"后台保存失败可报告且解除占用后可重试", [&]
    {
        auto store=save(L"worker-retry");
        HANDLE lock=CreateFile(store.file().c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
        require(lock!=INVALID_HANDLE_VALUE,"Cannot lock fixture.");
        PinSession session(store.file().parent_path()); std::wstring error;
        bool failed=!session.flush(session.submit({}),error); CloseHandle(lock);
        require(failed && !error.empty(),"Worker did not report failure.");
        require(session.flush(session.submit(snapshot),error) && error.empty(),"Worker could not retry.");
    });
    suite.test(L"未启用持久化的自动测试不触碰真实用户缓存", [&]
    { require(testing_ && !pinSession_,"Automated test unexpectedly enabled user persistence."); });
    suite.test(L"恢复后标注锁定，Tab 后仍可修改独立对象", [&]
    {
        auto old=windows_.size(); PinSnapshot state{{examplePin(100,100)},true}; restorePins(state);
        auto &w=*windows_.back();
        require(!w.showTools_ && !w.annotationsEditable() && w.doc_->marks.size()==7,"Restored annotations not locked.");
        auto before=w.doc_->marks[0].from;
        w.key(VK_TAB,false,false); require(w.annotationsEditable(),"Toolbar did not unlock restored annotations.");
        auto mark=w.doc_->marks[0]; mark.from.x+=20; w.doc_->replace(0,mark); w.changed();
        require(w.doc_->marks[0].from.x==before.x+20 && w.doc_->undo(),"Restored object not independently editable.");
        windows_.resize(old);
    });
    suite.test(L"恢复标注渲染与退出前逐像素一致", [&]
    {
        auto old=windows_.size(); auto source=examplePin(100,100);
        auto expected=renderAnnotations(graphics,*source.base,source.marks); restorePins({{source},true});
        require(windows_.back()->doc_->rendered->pixels==expected.pixels,"Restored raster differs or mosaic order changed.");
        windows_.resize(old);
    });
    suite.test(L"工具栏打开时恢复仍保留图片物理原点", [&]
    {
        auto old=windows_.size(); Document doc(Image(180,110)); pin(doc,{},PinPlacement{{320,240},1.25f});
        auto &w=*windows_.back(); w.key(VK_TAB,false,false); auto placement=w.imagePlacement();
        auto snapshot=pinSnapshot(); auto saved=snapshot.pins.back();
        require(saved.placement.imageOrigin.x==placement.imageOrigin.x && saved.placement.imageOrigin.y==placement.imageOrigin.y,
                "Saving toolbar state shifted image origin.");
        restorePins({{saved},true}); auto actual=windows_.back()->imagePlacement();
        require(actual.imageOrigin.x==placement.imageOrigin.x && actual.imageOrigin.y==placement.imageOrigin.y &&
                actual.zoom==placement.zoom,"Toolbar removal shifted restored image.");
        windows_.resize(old);
    });
    suite.test(L"损坏缓存启动时隔离保留，可建立新会话", [&]
    {
        auto dir=root/L"quarantine"; auto file=dir/L"session.dat";
        std::array<uint8_t,5> broken{1,2,3,4,5}; putFile(file,broken); startPinSession(dir);
        require(pinSession_!=nullptr,"New session was not available.");
        bool archived=false; for(auto const &entry:std::filesystem::directory_iterator(dir))
            if(entry.path().filename().wstring().starts_with(L"session-unreadable-")) archived=readFile(entry.path())==std::vector<uint8_t>(broken.begin(),broken.end());
        require(archived && savePins(true),"Unreadable cache not preserved."); pinSession_.reset();
    });
    suite.test(L"正常窗口移动、标注、隐藏和关闭触发保存计时器", [&]
    {
        auto old=windows_.size(); startPinSession(root/L"autosave");
        Document doc(Image(180,110)); pin(doc,{},PinPlacement{{120,140},1.f});
        auto &w=*windows_.back(); SetWindowPos(w.hwnd_,nullptr,240,260,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
        w.doc_->add({Tool::Text,{10,10},{100,30},{},L"自动保存 autosave",0x009955,3,18}); w.changed();
        togglePins();
        // Pump the real Windows timer instead of directly calling savePins().
        auto deadline=Clock::now()+std::chrono::seconds(3);
        bool timerFired=false;
        while(Clock::now()<deadline && !timerFired)
        {
            MSG msg{};
            while(PeekMessage(&msg,nullptr,0,0,PM_REMOVE))
            {
                if(msg.hwnd==root_ && msg.message==WM_TIMER && msg.wParam==4) timerFired=true;
                TranslateMessage(&msg); DispatchMessage(&msg);
            }
            if(!timerFired) MsgWaitForMultipleObjects(0,nullptr,FALSE,50,QS_ALLINPUT);
        }
        require(timerFired,"Autosave debounce timer did not run.");
        // Wait for the actual timer's generation through the worker destructor.
        pinSession_.reset();
        auto stored=PinSessionStore(root/L"autosave").load(graphics);
        require(stored.pins.size()==1 && stored.pins[0].window.left==240 && stored.pins[0].marks.size()==1 &&
                !stored.pins[0].visible,"Autosave did not capture changes.");
        pinSession_=std::make_unique<PinSession>(root/L"autosave");
        w.close(); require(savePins(true),"Closed pin save failed.");
        require(PinSessionStore(root/L"autosave").load(graphics).pins.empty(),"Closing pin retained it in next session.");
        pinSession_.reset(); windows_.resize(old); pinsVisible_=true;
    });
    suite.test(L"关机查询保存未确认文字，取消关机后仍可继续使用", [&]
    {
        auto old=windows_.size(); startPinSession(root/L"end-session");
        Document doc(Image(180,110)); pin(doc,{},PinPlacement{{120,140},1.f});
        auto &w=*windows_.back(); w.key(VK_TAB,false,false); w.beginText({10,10});
        SetWindowText(w.edit_,L"关机保存 shutdown");
        require(SendMessage(root_,WM_QUERYENDSESSION,0,ENDSESSION_LOGOFF)==TRUE,"Shutdown save was rejected.");
        auto stored=PinSessionStore(root/L"end-session").load(graphics);
        require(stored.pins.size()==1 && stored.pins[0].marks.size()==1 && !w.edit_,"Shutdown lost in-progress text.");
        SendMessage(root_,WM_ENDSESSION,FALSE,0);
        require(!quitting_ && w.hwnd_,"Cancelled shutdown closed the app.");
        pinSession_.reset(); windows_.resize(old);
    });
    // No pending WM_QUIT is sent by these tests. Release fixtures after a clean flush.
    pinSession_.reset();
    return suite.report(report,"pin-session");
}
int App::pinSessionCycle(std::wstring const &report, bool seed)
{
    Suite suite;
    auto root=std::filesystem::path(report).parent_path()/L"pin-session-process-cycle";
    auto cache=root/L"actual", expectedDir=root/L"expected";
    if(seed)
    {
        suite.test(L"独立进程正常退出前提交文字、保存多图并排除手动关闭贴图", [&]
        {
            // Test reports can be reused: overwrite fixture state explicitly.
            PinSessionStore(cache).save(graphics,{}); startPinSession(cache);
            auto areas=pinWorkAreas(); require(!areas.empty(),"No monitor available.");
            LONG x=areas[0].left+80,y=areas[0].top+90;
            Document doc(Image(320,170,0xf0f5fc)); doc.add({Tool::Arrow,{30,130},{220,70},{},L"",0xcc1155,3});
            pin(doc,{},PinPlacement{{x,y},1.5f}); auto &first=*windows_.back();
            first.key(VK_TAB,false,false); first.beginText({20,20});
            require(first.edit_!=nullptr,"Cannot create in-progress text editor.");
            SetWindowText(first.edit_,L"退出前尚未按 Enter · Restart text");
            pin(doc,{},PinPlacement{{x+400,y+260},.65f}); auto &second=*windows_.back();
            second.smoothZoom_=false; second.show(false);
            // A panned viewport must survive separately from the window position.
            second.view_.origin={-12.f,-8.f};
            pin(doc,{},PinPlacement{{x+20,y+30},1.f}); windows_.back()->close();
            auto firstOrigin=first.imagePlacement(),secondOrigin=second.imagePlacement();
            // quit() itself must finish the active editor, not only the test fixture.
            quit(); require(quitting_ && windows_.empty(),"Normal quit did not complete.");
            auto saved=PinSessionStore(cache).load(graphics);
            require(saved.pins.size()==2 && saved.pins[0].marks.size()==2 && saved.pins[0].marks[1].text.find(L"Restart text")!=std::wstring::npos,
                    "Quit dropped pending text or retained a closed pin.");
            require(saved.pins[0].placement.imageOrigin.x==firstOrigin.imageOrigin.x &&
                    saved.pins[0].placement.imageOrigin.y==firstOrigin.imageOrigin.y &&
                    saved.pins[1].placement.imageOrigin.x==secondOrigin.imageOrigin.x &&
                    saved.pins[1].placement.imageOrigin.y==secondOrigin.imageOrigin.y &&
                    !saved.pins[1].visible && !saved.pins[1].smoothZoom,"Quit changed image position or hidden state.");
            PinSessionStore(expectedDir).save(graphics,saved);
            pinSession_.reset();
            require(PinSessionStore(cache).load(graphics).pins.size()==2,"Shutdown cleanup overwrote session with empty windows.");
        });
    }
    else
    {
        suite.test(L"新进程启动恢复多图的像素、位置、缩放、平移、隐藏及标注", [&]
        {
            auto expected=PinSessionStore(expectedDir).load(graphics);
            require(expected.pins.size()==2,"Run the seed process first."); startPinSession(cache);
            auto restored=pinSnapshot(); require(restored.pins.size()==2,"Restart restored wrong number of pins.");
            for(size_t i=0;i<2;++i)
            {
                auto target=reachablePin(expected.pins[i],pinWorkAreas()); samePin(target,restored.pins[i]);
                require(!windows_[i]->showTools_ && !windows_[i]->annotationsEditable(),"Restart bypassed annotation lock.");
            }
        });
        suite.test(L"重启后关闭全部贴图再退出，不会在第三次启动复活", [&]
        {
            require(windows_.size()==2,"Restore process setup failed.");
            for(auto &window:windows_) window->close(); quit(); pinSession_.reset();
            require(PinSessionStore(cache).load(graphics).pins.empty(),"Closing all pins left old persisted images.");
        });
    }
    return suite.report(report,seed?"pin-session-seed":"pin-session-restore");
}
} // namespace snap
