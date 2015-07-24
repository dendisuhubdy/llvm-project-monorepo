//===-- Symbols.cpp ---------------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "lldb/Host/Symbols.h"

// C Includes
#include <dirent.h>
#include <pwd.h>
#include "lldb/Utility/SafeMachO.h"

// C++ Includes
// Other libraries and framework includes
#include <CoreFoundation/CoreFoundation.h>

// Project includes
#include "lldb/Core/ArchSpec.h"
#include "lldb/Core/DataBuffer.h"
#include "lldb/Core/DataExtractor.h"
#include "lldb/Core/Module.h"
#include "lldb/Core/ModuleSpec.h"
#include "lldb/Core/StreamString.h"
#include "lldb/Core/Timer.h"
#include "lldb/Core/UUID.h"
#include "lldb/Host/Endian.h"
#include "lldb/Host/Host.h"
#include "lldb/Symbol/ObjectFile.h"
#include "lldb/Utility/CleanUp.h"
#include "Host/macosx/cfcpp/CFCBundle.h"
#include "Host/macosx/cfcpp/CFCData.h"
#include "Host/macosx/cfcpp/CFCReleaser.h"
#include "Host/macosx/cfcpp/CFCString.h"
#include "mach/machine.h"

using namespace lldb;
using namespace lldb_private;
using namespace llvm::MachO;

#if !defined (__arm__) && !defined (__arm64__) && !defined (__aarch64__) // No DebugSymbols on the iOS devices
extern "C" {

CFURLRef DBGCopyFullDSYMURLForUUID (CFUUIDRef uuid, CFURLRef exec_url);
CFDictionaryRef DBGCopyDSYMPropertyLists (CFURLRef dsym_url);

}
#endif

int
LocateMacOSXFilesUsingDebugSymbols
(
    const ModuleSpec &module_spec,
    FileSpec *out_exec_fspec,   // If non-NULL, try and find the executable
    FileSpec *out_dsym_fspec    // If non-NULL try and find the debug symbol file
)
{
    int items_found = 0;

    if (out_exec_fspec)
        out_exec_fspec->Clear();

    if (out_dsym_fspec)
        out_dsym_fspec->Clear();

#if !defined (__arm__) && !defined (__arm64__) && !defined (__aarch64__) // No DebugSymbols on the iOS devices

    const UUID *uuid = module_spec.GetUUIDPtr();
    const ArchSpec *arch = module_spec.GetArchitecturePtr();

    if (uuid && uuid->IsValid())
    {
        // Try and locate the dSYM file using DebugSymbols first
        const UInt8 *module_uuid = (const UInt8 *)uuid->GetBytes();
        if (module_uuid != NULL)
        {
            CFCReleaser<CFUUIDRef> module_uuid_ref(::CFUUIDCreateWithBytes (NULL,
                                                                            module_uuid[0],
                                                                            module_uuid[1],
                                                                            module_uuid[2],
                                                                            module_uuid[3],
                                                                            module_uuid[4],
                                                                            module_uuid[5],
                                                                            module_uuid[6],
                                                                            module_uuid[7],
                                                                            module_uuid[8],
                                                                            module_uuid[9],
                                                                            module_uuid[10],
                                                                            module_uuid[11],
                                                                            module_uuid[12],
                                                                            module_uuid[13],
                                                                            module_uuid[14],
                                                                            module_uuid[15]));

            if (module_uuid_ref.get())
            {
                CFCReleaser<CFURLRef> exec_url;
                const FileSpec *exec_fspec = module_spec.GetFileSpecPtr();
                if (exec_fspec)
                {
                    char exec_cf_path[PATH_MAX];
                    if (exec_fspec->GetPath(exec_cf_path, sizeof(exec_cf_path)))
                        exec_url.reset(::CFURLCreateFromFileSystemRepresentation (NULL,
                                                                                  (const UInt8 *)exec_cf_path,
                                                                                  strlen(exec_cf_path),
                                                                                  FALSE));
                }

                CFCReleaser<CFURLRef> dsym_url (::DBGCopyFullDSYMURLForUUID(module_uuid_ref.get(), exec_url.get()));
                char path[PATH_MAX];

                if (dsym_url.get())
                {
                    if (out_dsym_fspec)
                    {
                        if (::CFURLGetFileSystemRepresentation (dsym_url.get(), true, (UInt8*)path, sizeof(path)-1))
                        {
                            out_dsym_fspec->SetFile(path, path[0] == '~');

                            if (out_dsym_fspec->GetFileType () == FileSpec::eFileTypeDirectory)
                            {
                                *out_dsym_fspec = Symbols::FindSymbolFileInBundle (*out_dsym_fspec, uuid, arch);
                                if (*out_dsym_fspec)
                                    ++items_found;
                            }
                            else
                            {
                                ++items_found;
                            }
                        }
                    }

                    if (out_exec_fspec)
                    {
                        bool success = false;
                        CFCReleaser<CFDictionaryRef> dict(::DBGCopyDSYMPropertyLists (dsym_url.get()));
                        CFDictionaryRef uuid_dict = NULL;
                        if (dict.get())
                        {
                            CFCString uuid_cfstr (uuid->GetAsString().c_str());
                            uuid_dict = static_cast<CFDictionaryRef>(::CFDictionaryGetValue (dict.get(), uuid_cfstr.get()));
                        }
                        if (uuid_dict)
                        {
                            CFStringRef exec_cf_path = static_cast<CFStringRef>(::CFDictionaryGetValue (uuid_dict, CFSTR("DBGSymbolRichExecutable")));
                            if (exec_cf_path && ::CFStringGetFileSystemRepresentation (exec_cf_path, path, sizeof(path)))
                            {
                                ++items_found;
                                out_exec_fspec->SetFile(path, path[0] == '~');
                                if (out_exec_fspec->Exists())
                                    success = true;
                            }
                        }

                        if (!success)
                        {
                            // No dictionary, check near the dSYM bundle for an executable that matches...
                            if (::CFURLGetFileSystemRepresentation (dsym_url.get(), true, (UInt8*)path, sizeof(path)-1))
                            {
                                char *dsym_extension_pos = ::strstr (path, ".dSYM");
                                if (dsym_extension_pos)
                                {
                                    *dsym_extension_pos = '\0';
                                    FileSpec file_spec (path, true);
                                    ModuleSpecList module_specs;
                                    ModuleSpec matched_module_spec;
                                    switch (file_spec.GetFileType())
                                    {
                                        case FileSpec::eFileTypeDirectory:  // Bundle directory?
                                            {
                                                CFCBundle bundle (path);
                                                CFCReleaser<CFURLRef> bundle_exe_url (bundle.CopyExecutableURL ());
                                                if (bundle_exe_url.get())
                                                {
                                                    if (::CFURLGetFileSystemRepresentation (bundle_exe_url.get(), true, (UInt8*)path, sizeof(path)-1))
                                                    {
                                                        FileSpec bundle_exe_file_spec (path, true);
                                                        if (ObjectFile::GetModuleSpecifications(bundle_exe_file_spec, 0, 0, module_specs) &&
                                                            module_specs.FindMatchingModuleSpec(module_spec, matched_module_spec))

                                                        {
                                                            ++items_found;
                                                            *out_exec_fspec = bundle_exe_file_spec;
                                                        }
                                                    }
                                                }
                                            }
                                            break;

                                        case FileSpec::eFileTypePipe:       // Forget pipes
                                        case FileSpec::eFileTypeSocket:     // We can't process socket files
                                        case FileSpec::eFileTypeInvalid:    // File doesn't exist...
                                            break;

                                        case FileSpec::eFileTypeUnknown:
                                        case FileSpec::eFileTypeRegular:
                                        case FileSpec::eFileTypeSymbolicLink:
                                        case FileSpec::eFileTypeOther:
                                            if (ObjectFile::GetModuleSpecifications(file_spec, 0, 0, module_specs) &&
                                                module_specs.FindMatchingModuleSpec(module_spec, matched_module_spec))

                                            {
                                                ++items_found;
                                                *out_exec_fspec = file_spec;
                                            }
                                            break;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
#endif // #if !defined (__arm__) && !defined (__arm64__) && !defined (__aarch64__)

    return items_found;
}

FileSpec
Symbols::FindSymbolFileInBundle (const FileSpec& dsym_bundle_fspec,
                                 const lldb_private::UUID *uuid,
                                 const ArchSpec *arch)
{
    char path[PATH_MAX];

    FileSpec dsym_fspec;

    if (dsym_bundle_fspec.GetPath(path, sizeof(path)))
    {
        ::strncat (path, "/Contents/Resources/DWARF", sizeof(path) - strlen(path) - 1);

        lldb_utility::CleanUp <DIR *, int> dirp (opendir(path), NULL, closedir);
        if (dirp.is_valid())
        {
            dsym_fspec.GetDirectory().SetCString(path);
            struct dirent* dp;
            while ((dp = readdir(dirp.get())) != NULL)
            {
                // Only search directories
                if (dp->d_type == DT_DIR || dp->d_type == DT_UNKNOWN)
                {
                    if (dp->d_namlen == 1 && dp->d_name[0] == '.')
                        continue;

                    if (dp->d_namlen == 2 && dp->d_name[0] == '.' && dp->d_name[1] == '.')
                        continue;
                }

                if (dp->d_type == DT_REG || dp->d_type == DT_UNKNOWN)
                {
                    dsym_fspec.GetFilename().SetCString(dp->d_name);
                    ModuleSpecList module_specs;
                    if (ObjectFile::GetModuleSpecifications(dsym_fspec, 0, 0, module_specs))
                    {
                        ModuleSpec spec;
                        for (size_t i = 0; i < module_specs.GetSize(); ++i)
                        {
                            assert(module_specs.GetModuleSpecAtIndex(i, spec));
                            if ((uuid == NULL || (spec.GetUUIDPtr() && spec.GetUUID() == *uuid)) &&
                                (arch == NULL || (spec.GetArchitecturePtr() && spec.GetArchitecture().IsCompatibleMatch(*arch))))
                            {
                                return dsym_fspec;
                            }
                        }
                    }
                }
            }
        }
    }
    dsym_fspec.Clear();
    return dsym_fspec;
}

static bool
GetModuleSpecInfoFromUUIDDictionary (CFDictionaryRef uuid_dict, ModuleSpec &module_spec)
{
    bool success = false;
    if (uuid_dict != NULL && CFGetTypeID (uuid_dict) == CFDictionaryGetTypeID ())
    {
        std::string str;
        CFStringRef cf_str;
        
        cf_str = (CFStringRef)CFDictionaryGetValue ((CFDictionaryRef) uuid_dict, CFSTR("DBGSymbolRichExecutable"));
        if (cf_str && CFGetTypeID (cf_str) == CFStringGetTypeID ())
        {
            if (CFCString::FileSystemRepresentation(cf_str, str))
                module_spec.GetFileSpec().SetFile (str.c_str(), true);
        }
        
        cf_str = (CFStringRef)CFDictionaryGetValue ((CFDictionaryRef) uuid_dict, CFSTR("DBGDSYMPath"));
        if (cf_str && CFGetTypeID (cf_str) == CFStringGetTypeID ())
        {
            if (CFCString::FileSystemRepresentation(cf_str, str))
            {
                module_spec.GetSymbolFileSpec().SetFile (str.c_str(), true);
                success = true;
            }
        }
        
        cf_str = (CFStringRef)CFDictionaryGetValue ((CFDictionaryRef) uuid_dict, CFSTR("DBGArchitecture"));
        if (cf_str && CFGetTypeID (cf_str) == CFStringGetTypeID ())
        {
            if (CFCString::FileSystemRepresentation(cf_str, str))
                module_spec.GetArchitecture().SetTriple(str.c_str());
        }

        std::string DBGBuildSourcePath;
        std::string DBGSourcePath;

        cf_str = (CFStringRef)CFDictionaryGetValue ((CFDictionaryRef) uuid_dict, CFSTR("DBGBuildSourcePath"));
        if (cf_str && CFGetTypeID (cf_str) == CFStringGetTypeID ())
        {
            CFCString::FileSystemRepresentation(cf_str, DBGBuildSourcePath);
        }

        cf_str = (CFStringRef)CFDictionaryGetValue ((CFDictionaryRef) uuid_dict, CFSTR("DBGSourcePath"));
        if (cf_str && CFGetTypeID (cf_str) == CFStringGetTypeID ())
        {
            CFCString::FileSystemRepresentation(cf_str, DBGSourcePath);
        }
        
        if (!DBGBuildSourcePath.empty() && !DBGSourcePath.empty())
        {
            module_spec.GetSourceMappingList().Append (ConstString(DBGBuildSourcePath.c_str()), ConstString(DBGSourcePath.c_str()), true);
        }
    }
    return success;
}


bool
Symbols::DownloadObjectAndSymbolFile (ModuleSpec &module_spec, bool force_lookup)
{
    bool success = false;
    const UUID *uuid_ptr = module_spec.GetUUIDPtr();
    const FileSpec *file_spec_ptr = module_spec.GetFileSpecPtr();

    // It's expensive to check for the DBGShellCommands defaults setting, only do it once per
    // lldb run and cache the result.  
    static bool g_have_checked_for_dbgshell_command = false;
    static const char *g_dbgshell_command = NULL;
    if (g_have_checked_for_dbgshell_command == false)
    {
        g_have_checked_for_dbgshell_command = true;
        CFTypeRef defaults_setting = CFPreferencesCopyAppValue (CFSTR ("DBGShellCommands"), CFSTR ("com.apple.DebugSymbols"));
        if (defaults_setting && CFGetTypeID (defaults_setting) == CFStringGetTypeID())
        { 
            char cstr_buf[PATH_MAX];
            if (CFStringGetCString ((CFStringRef) defaults_setting, cstr_buf, sizeof (cstr_buf), kCFStringEncodingUTF8))
            {
                g_dbgshell_command = strdup (cstr_buf);  // this malloc'ed memory will never be freed
            }
        }
        if (defaults_setting)
        {
            CFRelease (defaults_setting);
        }
    }

    // When g_dbgshell_command is NULL, the user has not enabled the use of an external program
    // to find the symbols, don't run it for them.
    if (force_lookup == false && g_dbgshell_command == NULL)
    {
        return false;
    }

    if (uuid_ptr || (file_spec_ptr && file_spec_ptr->Exists()))
    {
        static bool g_located_dsym_for_uuid_exe = false;
        static bool g_dsym_for_uuid_exe_exists = false;
        static char g_dsym_for_uuid_exe_path[PATH_MAX];
        if (!g_located_dsym_for_uuid_exe)
        {
            g_located_dsym_for_uuid_exe = true;
            const char *dsym_for_uuid_exe_path_cstr = getenv("LLDB_APPLE_DSYMFORUUID_EXECUTABLE");
            FileSpec dsym_for_uuid_exe_spec;
            if (dsym_for_uuid_exe_path_cstr)
            {
                dsym_for_uuid_exe_spec.SetFile(dsym_for_uuid_exe_path_cstr, true);
                g_dsym_for_uuid_exe_exists = dsym_for_uuid_exe_spec.Exists();
            }
            
            if (!g_dsym_for_uuid_exe_exists)
            {
                dsym_for_uuid_exe_spec.SetFile("/usr/local/bin/dsymForUUID", false);
                g_dsym_for_uuid_exe_exists = dsym_for_uuid_exe_spec.Exists();
                if (!g_dsym_for_uuid_exe_exists)
                {
                    long bufsize;
                    if ((bufsize = sysconf(_SC_GETPW_R_SIZE_MAX)) != -1)
                    {
                        char buffer[bufsize];
                        struct passwd pwd;
                        struct passwd *tilde_rc = NULL;
                        // we are a library so we need to use the reentrant version of getpwnam()
                        if (getpwnam_r ("rc", &pwd, buffer, bufsize, &tilde_rc) == 0 
                            && tilde_rc 
                            && tilde_rc->pw_dir)
                        {
                            std::string dsymforuuid_path(tilde_rc->pw_dir);
                            dsymforuuid_path += "/bin/dsymForUUID";
                            dsym_for_uuid_exe_spec.SetFile(dsymforuuid_path.c_str(), false);
                            g_dsym_for_uuid_exe_exists = dsym_for_uuid_exe_spec.Exists();
                        }
                    }
                }
            }
            if (!g_dsym_for_uuid_exe_exists && g_dbgshell_command != NULL)
            {
                dsym_for_uuid_exe_spec.SetFile(g_dbgshell_command, true);
                g_dsym_for_uuid_exe_exists = dsym_for_uuid_exe_spec.Exists();
            }

            if (g_dsym_for_uuid_exe_exists)
                dsym_for_uuid_exe_spec.GetPath (g_dsym_for_uuid_exe_path, sizeof(g_dsym_for_uuid_exe_path));
        }
        if (g_dsym_for_uuid_exe_exists)
        {
            std::string uuid_str;
            char file_path[PATH_MAX];
            file_path[0] = '\0';

            if (uuid_ptr)
                uuid_str = uuid_ptr->GetAsString();

            if (file_spec_ptr)
                file_spec_ptr->GetPath(file_path, sizeof(file_path));
            
            StreamString command;
            if (!uuid_str.empty())
                command.Printf("%s --ignoreNegativeCache --copyExecutable %s", g_dsym_for_uuid_exe_path, uuid_str.c_str());
            else if (file_path[0] != '\0')
                command.Printf("%s --ignoreNegativeCache --copyExecutable %s", g_dsym_for_uuid_exe_path, file_path);
            
            if (!command.GetString().empty())
            {
                int exit_status = -1;
                int signo = -1;
                std::string command_output;
                Error error = Host::RunShellCommand (command.GetData(),
                                                     NULL,              // current working directory
                                                     &exit_status,      // Exit status
                                                     &signo,            // Signal int *
                                                     &command_output,   // Command output
                                                     30,                // Large timeout to allow for long dsym download times
                                                     false);            // Don't run in a shell (we don't need shell expansion)
                if (error.Success() && exit_status == 0 && !command_output.empty())
                {
                    CFCData data (CFDataCreateWithBytesNoCopy (NULL,
                                                               (const UInt8 *)command_output.data(),
                                                               command_output.size(),
                                                               kCFAllocatorNull));
                    
                    CFCReleaser<CFDictionaryRef> plist((CFDictionaryRef)::CFPropertyListCreateFromXMLData (NULL, data.get(), kCFPropertyListImmutable, NULL));
                    
                    if (plist.get() && CFGetTypeID (plist.get()) == CFDictionaryGetTypeID ())
                    {
                        if (!uuid_str.empty())
                        {
                            CFCString uuid_cfstr(uuid_str.c_str());
                            CFDictionaryRef uuid_dict = (CFDictionaryRef)CFDictionaryGetValue (plist.get(), uuid_cfstr.get());
                            success = GetModuleSpecInfoFromUUIDDictionary (uuid_dict, module_spec);
                        }
                        else
                        {
                            const CFIndex num_values = ::CFDictionaryGetCount(plist.get());
                            if (num_values > 0)
                            {
                                std::vector<CFStringRef> keys (num_values, NULL);
                                std::vector<CFDictionaryRef> values (num_values, NULL);
                                ::CFDictionaryGetKeysAndValues(plist.get(), NULL, (const void **)&values[0]);
                                if (num_values == 1)
                                {
                                    return GetModuleSpecInfoFromUUIDDictionary (values[0], module_spec);
                                }
                                else
                                {
                                    for (CFIndex i=0; i<num_values; ++i)
                                    {
                                        ModuleSpec curr_module_spec;
                                        if (GetModuleSpecInfoFromUUIDDictionary (values[i], curr_module_spec))
                                        {
                                            if (module_spec.GetArchitecture().IsCompatibleMatch(curr_module_spec.GetArchitecture()))
                                            {
                                                module_spec = curr_module_spec;
                                                return true;
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    return success;
}

