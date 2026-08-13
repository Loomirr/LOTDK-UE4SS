#include <Constructs/Views/EnumerateView.hpp>
#include <File/File.hpp>

#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <ImageHlp.h>
#include <tchar.h>
#include <Helpers/String.hpp>
#include <Helpers/SysError.hpp>

using namespace RC;
namespace fs = std::filesystem;

using std::cerr;
using std::cout;
using std::endl;
using std::ifstream;
using std::ofstream;

using std::string;

struct ExportFunction
{
    uint16_t ordinal;
    bool is_named;
    string name;

    ExportFunction(uint16_t ordinal, bool is_named, string name) : ordinal(ordinal), is_named(is_named), name(name)
    {
    }
};

std::vector<ExportFunction> DumpExports(const fs::path& dll_path)
{
    auto dll_file = File::open(dll_path);
    const auto dll_file_map = dll_file.memory_map();

    ULONG export_directory_size = 0;
    IMAGE_EXPORT_DIRECTORY* export_directory =
            (IMAGE_EXPORT_DIRECTORY*)ImageDirectoryEntryToData(dll_file_map.data(), FALSE, IMAGE_DIRECTORY_ENTRY_EXPORT, &export_directory_size);

    if (export_directory == nullptr)
    {
        auto err_msg = to_string(SysError(GetLastError()));
        cerr << std::format("Failed to get export directory, reason: {}", err_msg) << '\n';
        return {};
    }

    IMAGE_DOS_HEADER* dos_header = (IMAGE_DOS_HEADER*)dll_file_map.data();
    IMAGE_NT_HEADERS* nt_header = (IMAGE_NT_HEADERS*)(dll_file_map.data() + dos_header->e_lfanew);

    DWORD* name_rvas = (DWORD*)ImageRvaToVa(nt_header, dll_file_map.data(), export_directory->AddressOfNames, NULL);
    DWORD* function_rvas = (DWORD*)ImageRvaToVa(nt_header, dll_file_map.data(), export_directory->AddressOfFunctions, NULL);
    uint16_t* ordinals = (uint16_t*)ImageRvaToVa(nt_header, dll_file_map.data(), export_directory->AddressOfNameOrdinals, NULL);

    std::vector<ExportFunction> exports;
    std::set<uint16_t> exported_ordinals;

    for (size_t i = 0; i < export_directory->NumberOfNames; i++)
    {
        std::string export_name = (char*)ImageRvaToVa(nt_header, dll_file_map.data(), name_rvas[i], NULL);
        uint16_t ordinal = ordinals[i] + 1;

        ExportFunction named_export(ordinal, true, export_name);
        exports.push_back(named_export);

        exported_ordinals.insert(ordinal);
    }

    for (size_t i = 0; i < export_directory->NumberOfFunctions; i++)
    {
        uint16_t ordinal = (uint16_t)(export_directory->Base + i);
        uint32_t function_rva = function_rvas[i];

        if (function_rva == 0) continue;
        if (exported_ordinals.contains(ordinal)) continue; // a named function for this ordinal was already exported

        ExportFunction ordinal_export(ordinal, false, std::format("ordinal{}", ordinal));
        exports.push_back(ordinal_export);
    }

    return exports;
}

std::vector<ExportFunction> ReadExportsFile(const fs::path& exp_path, fs::path& dll_path_out)
{
    dll_path_out.clear();
    ifstream exp_file(exp_path);
    string line;

    std::getline(exp_file, line);
    if (line.find("Path: ") != string::npos)
    {
        dll_path_out = line.substr(6);
    }

    if (dll_path_out.empty())
    {
        cerr << std::format("Failed to read export file (missing file path info)") << endl;
        return {};
    }

    std::vector<ExportFunction> exports;
    while (std::getline(exp_file, line))
    {
        std::istringstream s(line);

        uint16_t ordinal{};
        string export_name;
        s >> ordinal >> export_name;

        // invalid line - let's just hope functions with ordinal 0 don't exist
        if (ordinal == 0) continue;
        auto is_named = !export_name.empty();

        ExportFunction exp(ordinal, is_named, is_named ? export_name : std::format("ordinal{}", ordinal));
        exports.push_back(exp);
    }

    return exports;
}

int _tmain(int argc, TCHAR* argv[])
{
    if (argc != 3)
    {
        cerr << "Invalid arguments! Expected: proxy_generator.exe <input_dll_name_or_exports_file> <output_path>" << endl;
        return -1;
    }

    const fs::path input_file = argv[1];
    const fs::path output_path = argv[2];

    if (!fs::exists(input_file))
    {
        cerr << "Input file doesn't exist!\n" << endl;
        return -1;
    }

    fs::path input_dll = input_file;
    fs::path input_dll_name = input_file.filename();
    std::vector<ExportFunction> exports;

    if (input_file.extension() == ".exports")
    {
        cout << std::format("Generating a proxy using {}, output path: {}", input_dll_name.string(), output_path.string()) << endl;
        exports = ReadExportsFile(input_file, input_dll);
        input_dll_name = input_dll.filename();
    }
    else
    {
        cout << std::format("Generating a proxy for {}, output path: {}", input_dll.string(), output_path.string()) << endl;
        exports = DumpExports(input_dll);

        const auto exports_path = (output_path / input_dll_name).replace_extension("exports");
        ofstream exports_file(exports_path);
        exports_file << "Path: " << input_dll.string() << endl << endl;
        for (const auto [e, index] : exports | views::enumerate)
        {
            exports_file << std::format("{} {}", e.ordinal, e.is_named ? e.name : "") << endl;
        }
        exports_file.close();

        cout << std::format("Exports file generated at {}", exports_path.string()) << endl;
    }

    cout << std::format("Export count: {}", exports.size()) << endl;

    ofstream def_file((output_path / input_dll_name).replace_extension("def"));
    def_file << std::format("LIBRARY {}", fs::path(input_dll_name).replace_extension().string()) << endl;
    def_file << "EXPORTS" << endl;

    for (const auto [e, index] : exports | views::enumerate)
    {
        def_file << std::format("  {}=f{} @{}", e.name, index, e.ordinal) << endl;
    }
    def_file.close();

    ofstream asm_file((output_path / input_dll_name).replace_extension("asm"));
    asm_file << ".code" << endl;
    asm_file << "extern mProcs:QWORD" << endl;

    for (const auto [e, index] : exports | views::enumerate)
    {
        asm_file << std::format("f{} proc", index) << endl;
        asm_file << std::format("  jmp mProcs[8*{}]", index) << endl;
        asm_file << std::format("f{} endp", index) << endl;
    }

    asm_file << "end" << endl;
    asm_file.close();

    ofstream cpp_file(output_path / "dllmain.cpp");
    cpp_file << "#include <File/Macros.hpp>" << endl;
    cpp_file << endl;
    cpp_file << "#include <cstdint>" << endl;
    cpp_file << "#include <algorithm>" << endl;
    cpp_file << "#include <cstring>" << endl;
    cpp_file << "#include <cwctype>" << endl;
    cpp_file << "#include <fstream>" << endl;
    cpp_file << "#include <limits>" << endl;
    cpp_file << "#include <string>" << endl;
    cpp_file << "#include <vector>" << endl;
    cpp_file << endl;
    cpp_file << "#define WIN32_LEAN_AND_MEAN" << endl;
    cpp_file << "#include <Windows.h>" << endl;
    cpp_file << "#include <shellapi.h>" << endl;
    cpp_file << "#include <filesystem>" << endl;
    cpp_file << endl;
    cpp_file << "#pragma comment(lib, \"user32.lib\")" << endl;
    cpp_file << "#pragma comment(lib, \"shell32.lib\")" << endl;
    cpp_file << endl;

    cpp_file << "using namespace RC;" << endl;
    cpp_file << "namespace fs = std::filesystem;" << endl;
    cpp_file << endl;

    cpp_file << "HMODULE SOriginalDll = nullptr;" << endl;
    cpp_file << std::format("extern \"C\" uintptr_t mProcs[{}] = {{0}};", exports.size()) << endl;
    cpp_file << endl;

    // The game runs a Shipping UE build, so UE's -CmdLineFile support is compiled out. LOTDK Expanded
    // needs its static AssetRegistry plugins on the command line *before* Unreal's plugin manager
    // starts.  The proxy is the earliest reliable code we own, so generate the small, game-neutral
    // bootstrap here.  It is inert unless the dedicated RegistryPlugins folder exists.
    cpp_file << "struct LotdkExpandedProcessBasicInformation" << endl;
    cpp_file << "{" << endl;
    cpp_file << "    void* Reserved1;" << endl;
    cpp_file << "    void* PebBaseAddress;" << endl;
    cpp_file << "    void* Reserved2[2];" << endl;
    cpp_file << "    ULONG_PTR UniqueProcessId;" << endl;
    cpp_file << "    void* Reserved3;" << endl;
    cpp_file << "};" << endl;
    cpp_file << endl;
    cpp_file << "struct LotdkExpandedPebPrefix" << endl;
    cpp_file << "{" << endl;
    cpp_file << "    BYTE InheritedAddressSpace;" << endl;
    cpp_file << "    BYTE ReadImageFileExecOptions;" << endl;
    cpp_file << "    BYTE BeingDebugged;" << endl;
    cpp_file << "    BYTE BitField;" << endl;
    cpp_file << "    void* Mutant;" << endl;
    cpp_file << "    void* ImageBaseAddress;" << endl;
    cpp_file << "    void* Ldr;" << endl;
    cpp_file << "    struct LotdkExpandedRtlUserProcessParametersPrefix* ProcessParameters;" << endl;
    cpp_file << "};" << endl;
    cpp_file << endl;
    cpp_file << "struct LotdkExpandedUnicodeString" << endl;
    cpp_file << "{" << endl;
    cpp_file << "    USHORT Length;" << endl;
    cpp_file << "    USHORT MaximumLength;" << endl;
    cpp_file << "    PWSTR Buffer;" << endl;
    cpp_file << "};" << endl;
    cpp_file << endl;
    cpp_file << "struct LotdkExpandedRtlUserProcessParametersPrefix" << endl;
    cpp_file << "{" << endl;
    cpp_file << "    BYTE Reserved1[16];" << endl;
    cpp_file << "    void* Reserved2[10];" << endl;
    cpp_file << "    LotdkExpandedUnicodeString ImagePathName;" << endl;
    cpp_file << "    LotdkExpandedUnicodeString CommandLine;" << endl;
    cpp_file << "};" << endl;
    cpp_file << endl;
    cpp_file << "using LotdkExpandedNtQueryInformationProcess = LONG (NTAPI*)(HANDLE, ULONG, void*, ULONG, ULONG*);" << endl;
    cpp_file << endl;
    cpp_file << "static LPWSTR s_lotdk_expanded_command_line_override = nullptr;" << endl;
    cpp_file << "static decltype(&GetCommandLineW) s_lotdk_expanded_real_get_command_line_w = nullptr;" << endl;
    cpp_file << endl;
    cpp_file << "LPWSTR WINAPI lotdk_expanded_get_command_line_w()" << endl;
    cpp_file << "{" << endl;
    cpp_file << "    if (s_lotdk_expanded_command_line_override)" << endl;
    cpp_file << "    {" << endl;
    cpp_file << "        return s_lotdk_expanded_command_line_override;" << endl;
    cpp_file << "    }" << endl;
    cpp_file << "    return s_lotdk_expanded_real_get_command_line_w ? s_lotdk_expanded_real_get_command_line_w() : GetCommandLineW();" << endl;
    cpp_file << "}" << endl;
    cpp_file << endl;
    cpp_file << "std::wstring lotdk_expanded_lowercase(std::wstring value)" << endl;
    cpp_file << "{" << endl;
    cpp_file << "    for (auto& character : value)" << endl;
    cpp_file << "    {" << endl;
    cpp_file << "        character = static_cast<wchar_t>(std::towlower(character));" << endl;
    cpp_file << "    }" << endl;
    cpp_file << "    return value;" << endl;
    cpp_file << "}" << endl;
    cpp_file << endl;
    cpp_file << "fs::path lotdk_expanded_registry_plugins_root(HMODULE moduleHandle)" << endl;
    cpp_file << "{" << endl;
    cpp_file << "    wchar_t moduleFilenameBuffer[MAX_PATH]{};" << endl;
    cpp_file << "    if (GetModuleFileNameW(moduleHandle, moduleFilenameBuffer, MAX_PATH) == 0)" << endl;
    cpp_file << "    {" << endl;
    cpp_file << "        return {};" << endl;
    cpp_file << "    }" << endl;
    cpp_file << "    return fs::path(moduleFilenameBuffer).parent_path() / L\"ue4ss\" / L\"LOTDKExpanded\" / L\"RegistryPlugins\";" << endl;
    cpp_file << "}" << endl;
    cpp_file << endl;
    cpp_file << "void write_lotdk_expanded_bootstrap_status(const fs::path& root, const std::wstring& message) noexcept" << endl;
    cpp_file << "{" << endl;
    cpp_file << "    try" << endl;
    cpp_file << "    {" << endl;
    cpp_file << "        std::wofstream status(root.parent_path() / L\"LastPluginBootstrap.txt\", std::ios::trunc);" << endl;
    cpp_file << "        if (status)" << endl;
    cpp_file << "        {" << endl;
    cpp_file << "            status << message << L'\\n';" << endl;
    cpp_file << "        }" << endl;
    cpp_file << "    }" << endl;
    cpp_file << "    catch (...)" << endl;
    cpp_file << "    {" << endl;
    cpp_file << "        // Diagnostics must never block game startup." << endl;
    cpp_file << "    }" << endl;
    cpp_file << "}" << endl;
    cpp_file << endl;
    cpp_file << "LotdkExpandedRtlUserProcessParametersPrefix* get_lotdk_expanded_process_parameters()" << endl;
    cpp_file << "{" << endl;
    cpp_file << "    auto ntdll = GetModuleHandleW(L\"ntdll.dll\");" << endl;
    cpp_file << "    auto query = ntdll ? reinterpret_cast<LotdkExpandedNtQueryInformationProcess>(GetProcAddress(ntdll, \"NtQueryInformationProcess\")) : nullptr;" << endl;
    cpp_file << "    if (!query)" << endl;
    cpp_file << "    {" << endl;
    cpp_file << "        return nullptr;" << endl;
    cpp_file << "    }" << endl;
    cpp_file << endl;
    cpp_file << "    LotdkExpandedProcessBasicInformation processInformation{};" << endl;
    cpp_file << "    if (query(GetCurrentProcess(), 0, &processInformation, sizeof(processInformation), nullptr) < 0 || !processInformation.PebBaseAddress)" << endl;
    cpp_file << "    {" << endl;
    cpp_file << "        return nullptr;" << endl;
    cpp_file << "    }" << endl;
    cpp_file << endl;
    cpp_file << "    return reinterpret_cast<LotdkExpandedPebPrefix*>(processInformation.PebBaseAddress)->ProcessParameters;" << endl;
    cpp_file << "}" << endl;
    cpp_file << endl;
    cpp_file << "bool patch_lotdk_expanded_game_get_command_line_import(LPWSTR replacement)" << endl;
    cpp_file << "{" << endl;
    cpp_file << "    if (!replacement)" << endl;
    cpp_file << "    {" << endl;
    cpp_file << "        return false;" << endl;
    cpp_file << endl;
    cpp_file << "    }" << endl;
    cpp_file << "    auto* module = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));" << endl;
    cpp_file << "    if (!module)" << endl;
    cpp_file << "    {" << endl;
    cpp_file << "        return false;" << endl;
    cpp_file << "    }" << endl;
    cpp_file << endl;
    cpp_file << "    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(module);" << endl;
    cpp_file << "    if (dos->e_magic != IMAGE_DOS_SIGNATURE)" << endl;
    cpp_file << "    {" << endl;
    cpp_file << "        return false;" << endl;
    cpp_file << "    }" << endl;
    cpp_file << "    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(module + dos->e_lfanew);" << endl;
    cpp_file << "    if (nt->Signature != IMAGE_NT_SIGNATURE)" << endl;
    cpp_file << "    {" << endl;
    cpp_file << "        return false;" << endl;
    cpp_file << "    }" << endl;
    cpp_file << endl;
    cpp_file << "    const IMAGE_DATA_DIRECTORY& importDirectory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];" << endl;
    cpp_file << "    if (!importDirectory.VirtualAddress)" << endl;
    cpp_file << "    {" << endl;
    cpp_file << "        return false;" << endl;
    cpp_file << endl;
    cpp_file << "    }" << endl;
    cpp_file << "    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(module + importDirectory.VirtualAddress);" << endl;
    cpp_file << "    for (; descriptor->Name; ++descriptor)" << endl;
    cpp_file << "    {" << endl;
    cpp_file << "        if (!descriptor->OriginalFirstThunk || !descriptor->FirstThunk)" << endl;
    cpp_file << "        {" << endl;
    cpp_file << "            continue;" << endl;
    cpp_file << "        }" << endl;
    cpp_file << endl;
    cpp_file << "        auto* originalThunk = reinterpret_cast<IMAGE_THUNK_DATA64*>(module + descriptor->OriginalFirstThunk);" << endl;
    cpp_file << "        auto* firstThunk = reinterpret_cast<IMAGE_THUNK_DATA64*>(module + descriptor->FirstThunk);" << endl;
    cpp_file << "        for (; originalThunk->u1.AddressOfData; ++originalThunk, ++firstThunk)" << endl;
    cpp_file << "        {" << endl;
    cpp_file << "            if (IMAGE_SNAP_BY_ORDINAL64(originalThunk->u1.Ordinal))" << endl;
    cpp_file << "            {" << endl;
    cpp_file << "                continue;" << endl;
    cpp_file << "            }" << endl;
    cpp_file << "            auto* importByName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(module + originalThunk->u1.AddressOfData);" << endl;
    cpp_file << "            if (std::strcmp(reinterpret_cast<const char*>(importByName->Name), \"GetCommandLineW\") != 0)" << endl;
    cpp_file << "            {" << endl;
    cpp_file << "                continue;" << endl;
    cpp_file << "            }" << endl;
    cpp_file << endl;
    cpp_file << "            auto kernel32 = GetModuleHandleW(L\"kernel32.dll\");" << endl;
    cpp_file << "            s_lotdk_expanded_real_get_command_line_w = kernel32 ? reinterpret_cast<decltype(&GetCommandLineW)>(GetProcAddress(kernel32, \"GetCommandLineW\")) : nullptr;" << endl;
    cpp_file << "            if (!s_lotdk_expanded_real_get_command_line_w)" << endl;
    cpp_file << "            {" << endl;
    cpp_file << "                return false;" << endl;
    cpp_file << "            }" << endl;
    cpp_file << endl;
    cpp_file << "            DWORD oldProtection = 0;" << endl;
    cpp_file << "            auto* importSlot = reinterpret_cast<void**>(&firstThunk->u1.Function);" << endl;
    cpp_file << "            if (!VirtualProtect(importSlot, sizeof(*importSlot), PAGE_READWRITE, &oldProtection))" << endl;
    cpp_file << "            {" << endl;
    cpp_file << "                return false;" << endl;
    cpp_file << "            }" << endl;
    cpp_file << endl;
    cpp_file << "            s_lotdk_expanded_command_line_override = replacement;" << endl;
    cpp_file << "            *importSlot = reinterpret_cast<void*>(&lotdk_expanded_get_command_line_w);" << endl;
    cpp_file << "            DWORD ignoredProtection = 0;" << endl;
    cpp_file << "            VirtualProtect(importSlot, sizeof(*importSlot), oldProtection, &ignoredProtection);" << endl;
    cpp_file << "            return true;" << endl;
    cpp_file << "        }" << endl;
    cpp_file << "    }" << endl;
    cpp_file << endl;
    cpp_file << "    return false;" << endl;
    cpp_file << "}" << endl;
    cpp_file << endl;
    cpp_file << "void apply_lotdk_expanded_registry_plugin_bootstrap(HMODULE moduleHandle) noexcept" << endl;
    cpp_file << "{" << endl;
    cpp_file << "    try" << endl;
    cpp_file << "    {" << endl;
    cpp_file << "        const fs::path root = lotdk_expanded_registry_plugins_root(moduleHandle);" << endl;
    cpp_file << "        std::error_code error;" << endl;
    cpp_file << "        if (root.empty() || !fs::is_directory(root, error))" << endl;
    cpp_file << "        {" << endl;
    cpp_file << "            return;" << endl;
    cpp_file << "        }" << endl;
    cpp_file << endl;
    cpp_file << "        std::vector<fs::path> descriptors;" << endl;
    cpp_file << "        for (const auto& entry : fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, error))" << endl;
    cpp_file << "        {" << endl;
    cpp_file << "            if (error)" << endl;
    cpp_file << "            {" << endl;
    cpp_file << "                error.clear();" << endl;
    cpp_file << "                continue;" << endl;
    cpp_file << "            }" << endl;
    cpp_file << "            if (entry.is_regular_file(error) && !error && entry.path().extension() == L\".uplugin\")" << endl;
    cpp_file << "            {" << endl;
    cpp_file << "                descriptors.push_back(entry.path());" << endl;
    cpp_file << "            }" << endl;
    cpp_file << "            error.clear();" << endl;
    cpp_file << "        }" << endl;
    cpp_file << endl;
    // Config-bearing plugins must be supplied first. LOTDK Expanded's core plugin
    // extends the PawnMetaData scan roots through Config\\Game.ini; registry-only
    // plugins then contribute the individual DCMD rows.  A plain alphabetical
    // order can place an individual registry plugin before the core plugin.
    cpp_file << "        std::sort(descriptors.begin(), descriptors.end(), [](const fs::path& left, const fs::path& right)" << endl;
    cpp_file << "        {" << endl;
    cpp_file << "            std::error_code leftError;" << endl;
    cpp_file << "            std::error_code rightError;" << endl;
    cpp_file << "            const bool leftHasGameIni = fs::is_regular_file(left.parent_path() / L\"Config\" / L\"Game.ini\", leftError);" << endl;
    cpp_file << "            const bool rightHasGameIni = fs::is_regular_file(right.parent_path() / L\"Config\" / L\"Game.ini\", rightError);" << endl;
    cpp_file << "            if (leftHasGameIni != rightHasGameIni)" << endl;
    cpp_file << "            {" << endl;
    cpp_file << "                return leftHasGameIni;" << endl;
    cpp_file << "            }" << endl;
    cpp_file << "            return lotdk_expanded_lowercase(left.wstring()) < lotdk_expanded_lowercase(right.wstring());" << endl;
    cpp_file << "        });" << endl;
    cpp_file << "        // Registry destinations are normally one descriptor per plugin.  Keep" << endl;
    cpp_file << "        // the shallow, sorted descriptor if an interrupted older install left" << endl;
    cpp_file << "        // a nested duplicate behind; passing a plugin twice can corrupt the" << endl;
    cpp_file << "        // AssetRegistry discovery state before the game reaches its menu." << endl;
    cpp_file << "        std::vector<fs::path> uniqueDescriptors;" << endl;
    cpp_file << "        std::vector<std::wstring> seenPluginNames;" << endl;
    cpp_file << "        for (const fs::path& descriptor : descriptors)" << endl;
    cpp_file << "        {" << endl;
    cpp_file << "            const std::wstring pluginName = lotdk_expanded_lowercase(descriptor.stem().wstring());" << endl;
    cpp_file << "            if (std::find(seenPluginNames.begin(), seenPluginNames.end(), pluginName) != seenPluginNames.end())" << endl;
    cpp_file << "            {" << endl;
    cpp_file << "                continue;" << endl;
    cpp_file << "            }" << endl;
    cpp_file << "            seenPluginNames.push_back(pluginName);" << endl;
    cpp_file << "            uniqueDescriptors.push_back(descriptor);" << endl;
    cpp_file << "        }" << endl;
    cpp_file << "        descriptors = std::move(uniqueDescriptors);" << endl;
    cpp_file << "        if (descriptors.empty())" << endl;
    cpp_file << "        {" << endl;
    cpp_file << "            write_lotdk_expanded_bootstrap_status(root, L\"LOTDK Expanded plugin bootstrap: no .uplugin descriptors found.\");" << endl;
    cpp_file << "            return;" << endl;
    cpp_file << "        }" << endl;
    cpp_file << endl;
    cpp_file << "        std::wstring descriptorOrder;" << endl;
    cpp_file << "        for (const fs::path& descriptor : descriptors)" << endl;
    cpp_file << "        {" << endl;
    cpp_file << "            if (!descriptorOrder.empty())" << endl;
    cpp_file << "            {" << endl;
    cpp_file << "                descriptorOrder += L\",\";" << endl;
    cpp_file << "            }" << endl;
    cpp_file << "            descriptorOrder += descriptor.stem().wstring();" << endl;
    cpp_file << "        }" << endl;
    cpp_file << endl;
    cpp_file << "        auto* parameters = get_lotdk_expanded_process_parameters();" << endl;
    cpp_file << "        if (!parameters || !parameters->CommandLine.Buffer)" << endl;
    cpp_file << "        {" << endl;
    cpp_file << "            write_lotdk_expanded_bootstrap_status(root, L\"LOTDK Expanded plugin bootstrap: process command line was unavailable.\");" << endl;
    cpp_file << "            return;" << endl;
    cpp_file << "        }" << endl;
    cpp_file << endl;
    cpp_file << "        std::wstring commandLine(parameters->CommandLine.Buffer, parameters->CommandLine.Length / sizeof(wchar_t));" << endl;
    cpp_file << "        std::wstring commandLineLower = lotdk_expanded_lowercase(commandLine);" << endl;
    cpp_file << "        std::wstring firstInjectedNeedle;" << endl;
    cpp_file << "        size_t injected = 0;" << endl;
    cpp_file << "        for (const fs::path& descriptor : descriptors)" << endl;
    cpp_file << "        {" << endl;
    cpp_file << "            const std::wstring pluginPath = fs::absolute(descriptor, error).wstring();" << endl;
    cpp_file << "            const std::wstring pluginName = descriptor.stem().wstring();" << endl;
    cpp_file << "            error.clear();" << endl;
    cpp_file << "            if (pluginPath.empty() || pluginName.empty())" << endl;
    cpp_file << "            {" << endl;
    cpp_file << "                continue;" << endl;
    cpp_file << "            }" << endl;
    cpp_file << endl;
    cpp_file << "            const std::wstring pluginArgument = L\" -PLUGIN=\\\"\" + pluginPath + L\"\\\" -EnablePlugins=\" + pluginName;" << endl;
    cpp_file << "            if (commandLineLower.find(lotdk_expanded_lowercase(L\"-plugin=\\\"\" + pluginPath + L\"\\\"\")) != std::wstring::npos)" << endl;
    cpp_file << "            {" << endl;
    cpp_file << "                continue;" << endl;
    cpp_file << "            }" << endl;
    cpp_file << endl;
    cpp_file << "            commandLine += pluginArgument;" << endl;
    cpp_file << "            commandLineLower += lotdk_expanded_lowercase(pluginArgument);" << endl;
    cpp_file << "            if (firstInjectedNeedle.empty())" << endl;
    cpp_file << "            {" << endl;
    cpp_file << "                firstInjectedNeedle = lotdk_expanded_lowercase(L\"-plugin=\\\"\" + pluginPath + L\"\\\"\");" << endl;
    cpp_file << "            }" << endl;
    cpp_file << "            ++injected;" << endl;
    cpp_file << "        }" << endl;
    cpp_file << endl;
    cpp_file << "        if (injected == 0)" << endl;
    cpp_file << "        {" << endl;
    cpp_file << "            write_lotdk_expanded_bootstrap_status(root, L\"LOTDK Expanded plugin bootstrap: all discovered plugins were already present on the command line.\");" << endl;
    cpp_file << "            return;" << endl;
    cpp_file << "        }" << endl;
    cpp_file << endl;
    cpp_file << "        const size_t bytes = (commandLine.size() + 1) * sizeof(wchar_t);" << endl;
    cpp_file << "        if (bytes > (std::numeric_limits<USHORT>::max)())" << endl;
    cpp_file << "        {" << endl;
    cpp_file << "            write_lotdk_expanded_bootstrap_status(root, L\"LOTDK Expanded plugin bootstrap: plugin arguments exceed the Windows command-line limit.\");" << endl;
    cpp_file << "            return;" << endl;
    cpp_file << "        }" << endl;
    cpp_file << endl;
    cpp_file << "        auto* replacement = static_cast<wchar_t*>(VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));" << endl;
    cpp_file << "        if (!replacement)" << endl;
    cpp_file << "        {" << endl;
    cpp_file << "            write_lotdk_expanded_bootstrap_status(root, L\"LOTDK Expanded plugin bootstrap: could not allocate command-line storage.\");" << endl;
    cpp_file << "            return;" << endl;
    cpp_file << "        }" << endl;
    cpp_file << endl;
    cpp_file << "        std::memcpy(replacement, commandLine.c_str(), bytes);" << endl;
    cpp_file << "        parameters->CommandLine.Buffer = replacement;" << endl;
    cpp_file << "        parameters->CommandLine.Length = static_cast<USHORT>(bytes - sizeof(wchar_t));" << endl;
    cpp_file << "        parameters->CommandLine.MaximumLength = static_cast<USHORT>(bytes);" << endl;
    cpp_file << "        const bool gameImportPatched = patch_lotdk_expanded_game_get_command_line_import(replacement);" << endl;
    cpp_file << "        const wchar_t* apiCommandLine = GetCommandLineW();" << endl;
    cpp_file << "        const bool apiVerification = apiCommandLine && lotdk_expanded_lowercase(apiCommandLine).find(firstInjectedNeedle) != std::wstring::npos;" << endl;
    cpp_file << "        write_lotdk_expanded_bootstrap_status(root, L\"LOTDK Expanded plugin bootstrap: injected \" + std::to_wstring(injected) + L\" plugin(s); order=\" + descriptorOrder + L\"; proxy-api-view=\" + (apiVerification ? L\"yes\" : L\"no\") + L\"; game-GetCommandLineW-import-patch=\" + (gameImportPatched ? L\"yes\" : L\"no\") + L\".\");" << endl;
    cpp_file << "    }" << endl;
    cpp_file << "    catch (...)" << endl;
    cpp_file << "    {" << endl;
    cpp_file << "        // Never allow optional suit discovery to interrupt the game loader." << endl;
    cpp_file << "    }" << endl;
    cpp_file << "}" << endl;
    cpp_file << endl;

    cpp_file << "void setup_functions()" << endl;
    cpp_file << "{" << endl;

    for (const auto [e, index] : exports | views::enumerate)
    {
        string getter = e.is_named ? std::format("\"{}\"", e.name) : std::format("MAKEINTRESOURCEA({})", e.ordinal);
        cpp_file << std::format("    mProcs[{}] = (uintptr_t)GetProcAddress(SOriginalDll, {});", index, getter) << endl;
    }

    cpp_file << "}" << endl;
    cpp_file << endl;

    cpp_file << "void load_original_dll()" << endl;
    cpp_file << "{" << endl;
    cpp_file << "    wchar_t path[MAX_PATH];" << endl;
    cpp_file << "    GetSystemDirectory(path, MAX_PATH);" << endl;
    cpp_file << endl;
    cpp_file << std::format("    std::wstring dll_path = std::wstring(path) + L\"\\\\{}\";", input_dll_name.string()) << endl;
    cpp_file << endl;
    cpp_file << "    SOriginalDll = LoadLibrary(dll_path.c_str());" << endl;
    cpp_file << "    if (!SOriginalDll)" << endl;
    cpp_file << "    {" << endl;
    cpp_file << "        MessageBox(nullptr, L\"Failed to load proxy DLL\", L\"UE4SS Error\", MB_OK | MB_ICONERROR);" << endl;
    cpp_file << "        ExitProcess(0);" << endl;
    cpp_file << "    }" << endl;
    cpp_file << "}" << endl;
    cpp_file << endl;

    cpp_file << "bool is_absolute_path(const std::string& path)" << endl;
    cpp_file << "{" << endl;
    cpp_file << "    return fs::path(path).is_absolute();" << endl;
    cpp_file << "}" << endl;
    cpp_file << endl;

    cpp_file << "bool should_disable_ue4ss()" << endl;
    cpp_file << "{" << endl;
    cpp_file << "    int argc = 0;" << endl;
    cpp_file << "    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);" << endl;
    cpp_file << "    if (!argv)" << endl;
    cpp_file << "    {" << endl;
    cpp_file << "        return false;" << endl;
    cpp_file << "    }" << endl;
    cpp_file << endl;
    cpp_file << "    bool disable = false;" << endl;
    cpp_file << "    for (int i = 0; i < argc; i++)" << endl;
    cpp_file << "    {" << endl;
    cpp_file << "        if (wcscmp(argv[i], L\"--disable-ue4ss\") == 0)" << endl;
    cpp_file << "        {" << endl;
    cpp_file << "            disable = true;" << endl;
    cpp_file << "            break;" << endl;
    cpp_file << "        }" << endl;
    cpp_file << "    }" << endl;
    cpp_file << endl;
    cpp_file << "    LocalFree(argv);" << endl;
    cpp_file << "    return disable;" << endl;
    cpp_file << "}" << endl;
    cpp_file << endl;

    cpp_file << "std::wstring get_ue4ss_path_from_args()" << endl;
    cpp_file << "{" << endl;
    cpp_file << "    int argc = 0;" << endl;
    cpp_file << "    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);" << endl;
    cpp_file << "    if (!argv)" << endl;
    cpp_file << "    {" << endl;
    cpp_file << "        return L\"\";" << endl;
    cpp_file << "    }" << endl;
    cpp_file << endl;
    cpp_file << "    std::wstring ue4ss_path;" << endl;
    cpp_file << "    for (int i = 0; i < argc - 1; i++)" << endl;
    cpp_file << "    {" << endl;
    cpp_file << "        if (wcscmp(argv[i], L\"--ue4ss-path\") == 0)" << endl;
    cpp_file << "        {" << endl;
    cpp_file << "            ue4ss_path = argv[i + 1];" << endl;
    cpp_file << "            break;" << endl;
    cpp_file << "        }" << endl;
    cpp_file << "    }" << endl;
    cpp_file << endl;
    cpp_file << "    LocalFree(argv);" << endl;
    cpp_file << "    return ue4ss_path;" << endl;
    cpp_file << "}" << endl;
    cpp_file << endl;

    cpp_file << "HMODULE load_ue4ss_dll(HMODULE moduleHandle)" << endl;
    cpp_file << "{" << endl;
    cpp_file << "    HMODULE hModule = nullptr;" << endl;
    cpp_file << "    wchar_t moduleFilenameBuffer[1024]{'\\0'};" << endl;
    cpp_file << "    GetModuleFileNameW(moduleHandle, moduleFilenameBuffer, sizeof(moduleFilenameBuffer) / sizeof(wchar_t));" << endl;
    cpp_file << "    const auto currentPath = std::filesystem::path(moduleFilenameBuffer).parent_path();" << endl;
    cpp_file << "    const fs::path ue4ssPath = currentPath / \"ue4ss\" / \"UE4SS.dll\";" << endl;
    cpp_file << endl;

    cpp_file << "    // Check for --ue4ss-path command line argument" << endl;
    cpp_file << "    std::wstring cmdLineUe4ssPath = get_ue4ss_path_from_args();" << endl;
    cpp_file << "    if (!cmdLineUe4ssPath.empty())" << endl;
    cpp_file << "    {" << endl;
    cpp_file << "        fs::path ue4ssArgPath = cmdLineUe4ssPath;" << endl;
    cpp_file << "        if (!ue4ssArgPath.is_absolute())" << endl;
    cpp_file << "        {" << endl;
    cpp_file << "            ue4ssArgPath = currentPath / ue4ssArgPath;" << endl;
    cpp_file << "        }" << endl;
    cpp_file << endl;
    cpp_file << "        // Attempt to load UE4SS.dll from the command line path" << endl;
    cpp_file << "        hModule = LoadLibrary(ue4ssArgPath.c_str());" << endl;
    cpp_file << "        if (hModule)" << endl;
    cpp_file << "        {" << endl;
    cpp_file << "            return hModule;" << endl;
    cpp_file << "        }" << endl;
    cpp_file << "    }" << endl;
    cpp_file << endl;

    cpp_file << "    // Check for override.txt" << endl;
    cpp_file << "    const fs::path overrideFilePath = currentPath / \"override.txt\";" << endl;
    cpp_file << "    if (fs::exists(overrideFilePath))" << endl;
    cpp_file << "    {" << endl;
    cpp_file << "        std::ifstream overrideFile(overrideFilePath);" << endl;
    cpp_file << "        std::string overridePath;" << endl;
    cpp_file << "        if (std::getline(overrideFile, overridePath))" << endl;
    cpp_file << "        {" << endl;
    cpp_file << "            fs::path ue4ssOverridePath = overridePath;" << endl;
    cpp_file << "            if (!is_absolute_path(overridePath))" << endl;
    cpp_file << "            {" << endl;
    cpp_file << "                ue4ssOverridePath = currentPath / overridePath;" << endl;
    cpp_file << "            }" << endl;
    cpp_file << endl;
    cpp_file << "            ue4ssOverridePath = ue4ssOverridePath / \"UE4SS.dll\";" << endl;
    cpp_file << endl;
    cpp_file << "            // Attempt to load UE4SS.dll from the override path" << endl;
    cpp_file << "            hModule = LoadLibrary(ue4ssOverridePath.c_str());" << endl;
    cpp_file << "            if (hModule)" << endl;
    cpp_file << "            {" << endl;
    cpp_file << "                return hModule;" << endl;
    cpp_file << "            }" << endl;
    cpp_file << "        }" << endl;
    cpp_file << "    }" << endl;
    cpp_file << endl;

    cpp_file << "    // Attempt to load UE4SS.dll from ue4ss directory" << endl;
    cpp_file << "    hModule = LoadLibrary(ue4ssPath.c_str());" << endl;
    cpp_file << "    if (!hModule)" << endl;
    cpp_file << "    {" << endl;
    cpp_file << "        // If loading from ue4ss directory fails, load from the current directory" << endl;
    cpp_file << "        hModule = LoadLibrary(L\"UE4SS.dll\");" << endl;
    cpp_file << "    }" << endl;
    cpp_file << endl;
    cpp_file << "    return hModule;" << endl;
    cpp_file << "}" << endl;
    cpp_file << endl;

    cpp_file << "BOOL WINAPI DllMain(HMODULE hInstDll, DWORD fdwReason, LPVOID lpvReserved)" << endl;
    cpp_file << "{" << endl;
    cpp_file << "    if (fdwReason == DLL_PROCESS_ATTACH)" << endl;
    cpp_file << "    {" << endl;
    cpp_file << "        load_original_dll();" << endl;
    cpp_file << "        setup_functions();" << endl;
    cpp_file << endl;
    cpp_file << "        // Check if UE4SS should be disabled via command line argument" << endl;
    cpp_file << "        if (should_disable_ue4ss())" << endl;
    cpp_file << "        {" << endl;
    cpp_file << "            // UE4SS is disabled, proxy will still forward calls to original DLL" << endl;
    cpp_file << "            return TRUE;" << endl;
    cpp_file << "        }" << endl;
    cpp_file << endl;
    cpp_file << "        // The LOTDK plugin bootstrap is intentionally after UE4SS safe mode." << endl;
    cpp_file << "        // Passing --disable-ue4ss therefore restores the unmodified game command line." << endl;
    cpp_file << "        apply_lotdk_expanded_registry_plugin_bootstrap(hInstDll);" << endl;
    cpp_file << endl;
    cpp_file << "        HMODULE hUE4SSDll = load_ue4ss_dll(hInstDll);" << endl;
    cpp_file << "        if (!hUE4SSDll)" << endl;
    cpp_file << "        {" << endl;
    cpp_file << "            MessageBox(nullptr, L\"Failed to load UE4SS.dll. Please see the docs on correct installation: "
                "https://docs.ue4ss.com/installation-guide\", L\"UE4SS Error\", MB_OK | MB_ICONERROR);"
             << endl;
    cpp_file << "            ExitProcess(0);" << endl;
    cpp_file << "        }" << endl;
    cpp_file << "    }" << endl;
    cpp_file << "    else if (fdwReason == DLL_PROCESS_DETACH)" << endl;
    cpp_file << "    {" << endl;
    cpp_file << "        FreeLibrary(SOriginalDll);" << endl;
    cpp_file << "    }" << endl;
    cpp_file << "    return TRUE;" << endl;
    cpp_file << "}" << endl;

    cpp_file.close();

    cout << "Finished generating!" << endl;

    return 0;
}
