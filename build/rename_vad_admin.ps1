$ErrorActionPreference = 'Continue'
$log = 'C:\Users\akram\Downloads\vb\build\rename_vad_admin.log'
function Log($m) { Add-Content -LiteralPath $log -Value $m }
Set-Content -LiteralPath $log -Value 'start'

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
using System.Security.AccessControl;
using System.Security.Principal;
using Microsoft.Win32;

public static class VadLabel {
  [StructLayout(LayoutKind.Sequential)]
  struct LUID { public uint Low; public int High; }
  [StructLayout(LayoutKind.Sequential)]
  struct LUID_AND_ATTRIBUTES { public LUID Luid; public uint Attr; }
  [StructLayout(LayoutKind.Sequential)]
  struct TOKEN_PRIVILEGES { public uint Count; public LUID_AND_ATTRIBUTES Priv; }

  [DllImport("advapi32.dll", SetLastError = true)]
  static extern bool OpenProcessToken(IntPtr h, uint acc, out IntPtr tok);
  [DllImport("kernel32.dll")]
  static extern IntPtr GetCurrentProcess();
  [DllImport("advapi32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
  static extern bool LookupPrivilegeValue(string sys, string name, out LUID luid);
  [DllImport("advapi32.dll", SetLastError = true)]
  static extern bool AdjustTokenPrivileges(IntPtr tok, bool dis, ref TOKEN_PRIVILEGES n, uint len, IntPtr prev, IntPtr ret);

  static void Enable(string name) {
    IntPtr tok;
    if (!OpenProcessToken(GetCurrentProcess(), 0x28, out tok))
      throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
    LUID luid;
    if (!LookupPrivilegeValue(null, name, out luid))
      throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
    var tp = new TOKEN_PRIVILEGES();
    tp.Count = 1;
    tp.Priv.Luid = luid;
    tp.Priv.Attr = 2;
    if (!AdjustTokenPrivileges(tok, false, ref tp, 0, IntPtr.Zero, IntPtr.Zero))
      throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
    int err = Marshal.GetLastWin32Error();
    if (err != 0) throw new System.ComponentModel.Win32Exception(err);
  }

  public static string Apply(string subKey, string newName) {
    try {
      Enable("SeTakeOwnershipPrivilege");
      Enable("SeRestorePrivilege");
      var openRights = RegistryRights.TakeOwnership | RegistryRights.ReadKey;
      using (var key = Registry.LocalMachine.OpenSubKey(subKey, RegistryKeyPermissionCheck.ReadWriteSubTree, openRights)) {
        if (key == null) return "open-owner-null";
        var sec = key.GetAccessControl(AccessControlSections.Owner);
        sec.SetOwner(new SecurityIdentifier(WellKnownSidType.BuiltinAdministratorsSid, null));
        key.SetAccessControl(sec);
      }
      using (var key = Registry.LocalMachine.OpenSubKey(subKey, RegistryKeyPermissionCheck.ReadWriteSubTree, RegistryRights.ChangePermissions | RegistryRights.ReadKey)) {
        if (key == null) return "open-dacl-null";
        var sec = key.GetAccessControl();
        sec.AddAccessRule(new RegistryAccessRule(
          new SecurityIdentifier(WellKnownSidType.BuiltinAdministratorsSid, null),
          RegistryRights.FullControl,
          InheritanceFlags.None,
          PropagationFlags.None,
          AccessControlType.Allow));
        key.SetAccessControl(sec);
      }
      using (var key = Registry.LocalMachine.OpenSubKey(subKey, true)) {
        if (key == null) return "open-write-null";
        string before = key.GetValue("{a45c254e-df1c-4efd-8020-67d146a850e0},2") as string;
        string driver = key.GetValue("{b3f8fa53-0004-438e-9003-51a46e139bfc},6") as string;
        key.SetValue("{a45c254e-df1c-4efd-8020-67d146a850e0},2", newName, RegistryValueKind.String);
        string got = key.GetValue("{a45c254e-df1c-4efd-8020-67d146a850e0},2") as string;
        return "before=" + before + " now=" + got + " inf=" + driver;
      }
    } catch (Exception ex) {
      return "ex " + ex.GetType().Name + " " + ex.Message;
    }
  }
}
"@

$map = @(
  'SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render\{bc15df3c-a41d-439b-aad8-3b87123065ac}\Properties|AnyMix Game',
  'SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render\{f767fbae-e008-4c27-ae29-ded399e0606b}\Properties|AnyMix Chat',
  'SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render\{c755a403-460e-477c-a2de-962b6fa8ac72}\Properties|AnyMix Music',
  'SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render\{ca632b26-a53e-4f30-be63-057824ca7e60}\Properties|AnyMix Aux',
  'SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render\{982bd150-a432-45c5-83c7-d98f2a680cf0}\Properties|AnyMix Add',
  'SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Capture\{4a189561-5d81-4612-949d-a2023cbf588a}\Properties|Mix 01',
  'SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Capture\{5c9fa389-a152-4908-8665-3cebeda533e9}\Properties|Mix 02',
  'SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Capture\{8e4e4a33-d329-4bb6-b282-bd02b27481a5}\Properties|Mix 03',
  'SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Capture\{06601816-a737-49e8-94a3-e1a498fab70f}\Properties|Mix 04',
  'SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Capture\{b7bd6219-e23e-4050-9f42-d4cb63728d1e}\Properties|Mix 05',
  'SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Capture\{9619ac4e-354e-4b8b-a940-c9f6b45f9059}\Properties|AnyMix B1',
  'SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Capture\{2107c68e-a588-4a9e-a2b4-f72e2a8f93a2}\Properties|AnyMix B2',
  'SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Capture\{7e2d0f01-d93a-4454-b64b-0e60d25374a8}\Properties|AnyMix B3',
  'SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Capture\{caf6c6c7-86ab-4bc5-91da-5cda4f106203}\Properties|AnyMix B4',
  'SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Capture\{ecdfc9de-bbcc-4e9a-8063-f4bcb6245aa8}\Properties|AnyMix B5'
)
foreach ($row in $map) {
  $parts = $row.Split('|')
  Log ($parts[1] + ' ' + [VadLabel]::Apply($parts[0], $parts[1]))
}
try {
  Restart-Service -Name AudioEndpointBuilder -Force -ErrorAction Stop
  Restart-Service -Name Audiosrv -Force -ErrorAction Stop
  Log 'services restarted'
} catch {
  Log ('service ' + $_.Exception.Message)
}
Log 'done'
