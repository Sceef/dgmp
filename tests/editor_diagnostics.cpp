#include "../src/diagnostics.h"
#include "../src/log.h"
#include <filesystem>
#include <iostream>
#include <cassert>
__declspec(noinline) bool Fault() {
    __try { *reinterpret_cast<volatile int*>(uintptr_t(0x1234))=1; return false; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return true; }
}
int main() {
    const auto root=std::filesystem::absolute(std::filesystem::path("build")/("diagnostics-test-"+std::to_string(GetCurrentProcessId())));
    std::filesystem::create_directories(root/"logs");
    dg::LogOpen((root/"logs"/"DGMP.log").string().c_str());
    diagnostics::Start(root,true);
    diagnostics::Phase("synthetic-test");
    assert(Fault()); // original SEH handling still runs; observer does not swallow exceptions
    assert(Fault()); // repeated fault at the same instruction must not produce a second dump
    diagnostics::SetEnabled(false);
    assert(Fault());
    size_t dumps=0;
    for(const auto& entry:std::filesystem::recursive_directory_iterator(root))
        if(entry.path().extension()==L".dmp") { ++dumps; assert(entry.file_size()>1024); }
    assert(dumps==1);
    std::cout << "PASS: exception dump, duplicate suppression, diagnostics off, original exception handler preserved\n";
    std::cout << "Artifacts: " << root << '\n';
}
