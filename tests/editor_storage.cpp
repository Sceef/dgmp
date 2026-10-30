#include "../src/editor_storage.h"
#include <cassert>
#include <iostream>
#include <thread>
using namespace editor_storage;
int main() {
    const auto dir = fs::absolute(fs::path("build") / ("editor-storage-test-" + std::to_string(GetCurrentProcessId())));
    const auto path = dir / "world.json";
    const std::string initial = "{\"logic\":{\"spawn\":{\"x\":1}}}";
    const std::string changed = "{\"logic\":{\"spawn\":{\"x\":2},\"office\":{\"x\":3}}}";
    assert(Write(path, initial));
    Document doc; doc.Load(path, initial);
    assert(doc.Save(initial,true)==Result::Unchanged);
    assert(!fs::exists(dir / ".autosave"));
    assert(doc.Save(changed,true)==Result::Saved);
    assert(Read(path).text==changed && Read(path.wstring()+L".bak").text==initial);
    assert(Read(dir/".autosave"/"world.json").text==changed);
    assert(!doc.Dirty(changed));
    // External edits win; the editor's unsaved work is still recoverable.
    const std::string external="{\"external\":true}";
    assert(Write(path,external));
    assert(doc.Save(initial,true)==Result::RecoveryOnly);
    assert(Read(path).text==external && Read(dir/".autosave"/"world.json").text==initial);
    assert(doc.Dirty(initial));
    // Manual save acknowledges replacement; a later autosave uses the new baseline.
    assert(doc.Save(initial,false)==Result::Saved);
    assert(doc.Save(changed,true)==Result::Saved);
    // Simulate antivirus/another editor holding the destination without delete sharing.
    HANDLE held=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    assert(held!=INVALID_HANDLE_VALUE);
    assert(doc.Save(initial,true)==Result::RecoveryOnly);
    assert(Read(path).text==changed && Read(dir/".autosave"/"world.json").text==initial);
    CloseHandle(held);
    assert(doc.Save(initial,true)==Result::Saved);
    // A directory at the destination is an I/O error, never a successful save.
    fs::create_directory(dir/"blocked.json");
    assert(!Write(dir/"blocked.json",changed));
    // Readers see an entire old or new document, never a partially-written JSON.
    std::atomic<bool> done{false}, torn{false};
    std::thread reader([&] { while(!done) { auto state=Read(path); if(state.ok && state.exists && state.text!=initial && state.text!=changed) torn=true; } });
    unsigned succeeded = 0, retried = 0;
    for(int n=0;n<40;++n) {
        bool written = false;
        for(int attempt=0;attempt<100 && !written;++attempt) {
            written = Write(path,n%2?initial:changed);
            if(!written) { ++retried; Sleep(1); }
        }
        assert(written); ++succeeded;
    }
    done=true; reader.join(); assert(!torn);
    Schedule schedule;
    for(int minutes : {1,5,10}) {
        schedule.Set(minutes,100);
        assert(!schedule.Due(100+minutes*60-0.01));
        assert(schedule.Due(100+minutes*60));
        assert(!schedule.Due(100+minutes*60+0.01));
    }
    schedule.Set(0,0); assert(!schedule.Due(100000));
    schedule.Set(999,0); assert(schedule.minutes==1 && schedule.Due(60));
    std::cout << "PASS: atomic save, backups, recovery, external conflict, locked file, write error, concurrent readers, 1/5/10-minute scheduling\n";
    std::cout << "Concurrent replacements=" << succeeded << " transient sharing retries=" << retried << '\n';
    std::cout << "Artifacts: " << dir << '\n';
}
