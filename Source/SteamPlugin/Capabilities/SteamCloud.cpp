#include <SteamPlugin/SteamPluginPCH.h>

#include <Foundation/IO/MemoryStream.h>
#include <Foundation/Utilities/ConversionUtils.h>
#include <SteamPlugin/SteamPlatformBackend.h>

// Steam Cloud through ISteamRemoteStorage. One file per slot, named "sav_<project>_<profile>_<slot>.plsave" with the
// uuids as 32 hex digits and "profile" for the independent profile document. The revision bookkeeping lives in a
// header in front of the envelope, so a record is one file and one write:
//
//   u32 magic 'PLSC' | u32 version | uuid revision | uuid parent | u8 deleted | u32 envelope size | envelope
//
// Tombstones keep the file with the deleted flag set and no envelope. Steam applies no server-side compare, so the
// revision check is read-then-write on the client; the save system's ledger keeps that honest between machines.
//
// These calls run on an I/O task. They touch no backend state, only ISteamRemoteStorage, whose synchronous file
// calls are thread-safe.

static constexpr plUInt32 s_uiCloudMagic = 0x43534C50; // 'PLSC'
static constexpr plUInt32 s_uiCloudVersion = 1;
static constexpr plUInt32 s_uiCloudHeaderSize = 4 + 4 + 16 + 16 + 1 + 4;
static constexpr plInt32 s_iCloudFileLimit = 100 * 1024 * 1024; // ISteamRemoteStorage::FileWrite maximum
static constexpr const char* s_szCloudSuffix = ".plsave";
static constexpr const char* s_szCloudProfileSlot = "profile";

static void AppendUuidHex(plStringBuilder& ref_sTarget, const plUuid& uuid)
{
  plUInt64 uiLow = 0, uiHigh = 0;
  uuid.GetValues(uiLow, uiHigh);
  ref_sTarget.AppendFormat("{0}{1}", plArgU(uiHigh, 16, true, 16), plArgU(uiLow, 16, true, 16));
}

static bool ParseUuidHex(plStringView sHex, plUuid& out_uuid)
{
  if (sHex.GetElementCount() != 32)
    return false;

  plUInt64 uiHigh = 0, uiLow = 0;
  if (plConversionUtils::ConvertHexStringToUInt64(plStringView(sHex.GetStartPointer(), sHex.GetStartPointer() + 16), uiHigh).Failed())
    return false;
  if (plConversionUtils::ConvertHexStringToUInt64(plStringView(sHex.GetStartPointer() + 16, sHex.GetEndPointer()), uiLow).Failed())
    return false;

  out_uuid = plUuid(uiLow, uiHigh);
  return true;
}

static void AppendSanitized(plStringBuilder& ref_sTarget, plStringView sText)
{
  // Steam Cloud file names are case-insensitive and limited to a safe character set; the project name is user text.
  for (const char* p = sText.GetStartPointer(); p < sText.GetEndPointer(); ++p)
  {
    const char c = *p;
    const bool bSafe = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-';
    ref_sTarget.Append(bSafe ? c : '_');
  }
}

bool plSteamPlatformBackend::IsCloudAvailable() const
{
  ISteamRemoteStorage* pStorage = SteamRemoteStorage();
  return m_bInitialized && pStorage != nullptr && pStorage->IsCloudEnabledForAccount() && pStorage->IsCloudEnabledForApp();
}

plString plSteamPlatformBackend::CloudFilePrefix(plStringView sProject, const plUuid& profile) const
{
  plStringBuilder s = "sav_";
  AppendSanitized(s, sProject);
  s.Append("_");
  AppendUuidHex(s, profile);
  s.Append("_");
  s.ToLower();
  return s;
}

plString plSteamPlatformBackend::CloudFileName(const plSaveGameCloudKey& key) const
{
  plStringBuilder s = CloudFilePrefix(key.m_sProject, key.m_Profile);
  if (key.m_Slot.IsValid())
    AppendUuidHex(s, key.m_Slot);
  else
    s.Append(s_szCloudProfileSlot);
  s.Append(s_szCloudSuffix);
  s.ToLower();
  return s;
}

bool plSteamPlatformBackend::ReadCloudFile(const char* szName, plSaveGameCloudRecord& out_record, plSaveGameError& out_error) const
{
  ISteamRemoteStorage* pStorage = SteamRemoteStorage();

  const plInt32 iSize = pStorage->GetFileSize(szName);
  if (iSize < static_cast<plInt32>(s_uiCloudHeaderSize))
  {
    out_error = plSaveGameError::CorruptData;
    return false;
  }

  plDynamicArray<plUInt8> buffer;
  buffer.SetCountUninitialized(static_cast<plUInt32>(iSize));

  const plInt32 iRead = pStorage->FileRead(szName, buffer.GetData(), iSize);
  if (iRead != iSize)
  {
    out_error = plSaveGameError::IoError;
    return false;
  }

  plRawMemoryStreamReader reader(buffer.GetData(), buffer.GetCount());

  plUInt32 uiMagic = 0, uiVersion = 0;
  reader.ReadDWordValue(&uiMagic).IgnoreResult();
  reader.ReadDWordValue(&uiVersion).IgnoreResult();
  if (uiMagic != s_uiCloudMagic || uiVersion != s_uiCloudVersion)
  {
    out_error = plSaveGameError::CorruptData;
    return false;
  }

  reader >> out_record.m_Revision;
  reader >> out_record.m_ParentRevision;

  plUInt8 uiDeleted = 0;
  reader.ReadBytes(&uiDeleted, 1);
  out_record.m_bDeleted = uiDeleted != 0;

  plUInt32 uiEnvelopeSize = 0;
  reader.ReadDWordValue(&uiEnvelopeSize).IgnoreResult();
  if (uiEnvelopeSize != buffer.GetCount() - s_uiCloudHeaderSize)
  {
    out_error = plSaveGameError::CorruptData;
    return false;
  }

  out_record.m_Envelope.SetCountUninitialized(uiEnvelopeSize);
  if (uiEnvelopeSize > 0 && reader.ReadBytes(out_record.m_Envelope.GetData(), uiEnvelopeSize) != uiEnvelopeSize)
  {
    out_error = plSaveGameError::CorruptData;
    return false;
  }

  out_error = plSaveGameError::Success;
  return true;
}

plSaveGameError plSteamPlatformBackend::Read(const plSaveGameCloudKey& key, plSaveGameCloudRecord& out_record)
{
  if (!IsCloudAvailable())
    return plSaveGameError::IoError;

  const plString sName = CloudFileName(key);
  if (!SteamRemoteStorage()->FileExists(sName.GetData()))
    return plSaveGameError::NotFound;

  plSaveGameError error = plSaveGameError::Success;
  ReadCloudFile(sName.GetData(), out_record, error);
  return error;
}

plSaveGameError plSteamPlatformBackend::Publish(const plSaveGameCloudKey& key, const plSaveGameCloudRecord& record, const plUuid& expectedRemoteRevision)
{
  if (!IsCloudAvailable())
    return plSaveGameError::IoError;

  if (!record.m_Revision.IsValid())
    return plSaveGameError::InvalidArgument;

  ISteamRemoteStorage* pStorage = SteamRemoteStorage();
  const plString sName = CloudFileName(key);

  if (pStorage->FileExists(sName.GetData()))
  {
    plSaveGameCloudRecord remote;
    plSaveGameError error = plSaveGameError::Success;
    if (!ReadCloudFile(sName.GetData(), remote, error))
      return error;

    // The same revision again is a no-op, whatever the caller expected.
    if (remote.m_Revision == record.m_Revision && remote.m_bDeleted == record.m_bDeleted && remote.m_Envelope == record.m_Envelope)
      return plSaveGameError::Success;

    if (remote.m_Revision != expectedRemoteRevision)
      return plSaveGameError::Conflict;
  }
  else if (expectedRemoteRevision.IsValid())
  {
    return plSaveGameError::Conflict;
  }

  const plUInt32 uiEnvelopeSize = record.m_bDeleted ? 0 : record.m_Envelope.GetCount();
  if (static_cast<plInt64>(uiEnvelopeSize) + s_uiCloudHeaderSize > s_iCloudFileLimit)
    return plSaveGameError::LimitExceeded;

  plDefaultMemoryStreamStorage storage;
  plMemoryStreamWriter writer(&storage);
  writer.WriteDWordValue(&s_uiCloudMagic).IgnoreResult();
  writer.WriteDWordValue(&s_uiCloudVersion).IgnoreResult();
  writer << record.m_Revision;
  writer << record.m_ParentRevision;
  const plUInt8 uiDeleted = record.m_bDeleted ? 1 : 0;
  writer.WriteBytes(&uiDeleted, 1).IgnoreResult();
  writer.WriteDWordValue(&uiEnvelopeSize).IgnoreResult();
  if (uiEnvelopeSize > 0)
    writer.WriteBytes(record.m_Envelope.GetData(), uiEnvelopeSize).IgnoreResult();

  plDynamicArray<plUInt8> bytes;
  bytes.Reserve(storage.GetStorageSize32());
  plUInt64 uiOffset = 0;
  while (uiOffset < storage.GetStorageSize64())
  {
    const plArrayPtr<const plUInt8> range = storage.GetContiguousMemoryRange(uiOffset);
    if (range.IsEmpty())
      break;
    bytes.PushBackRange(range);
    uiOffset += range.GetCount();
  }

  if (!pStorage->FileWrite(sName.GetData(), bytes.GetData(), static_cast<plInt32>(bytes.GetCount())))
  {
    plLog::Warning("Steam: cloud write of '{0}' ({1} bytes) failed; quota or Cloud disabled.", sName, bytes.GetCount());
    return plSaveGameError::IoError;
  }

  return plSaveGameError::Success;
}

plSaveGameError plSteamPlatformBackend::Enumerate(plStringView sProject, const plUuid& profile, plDynamicArray<plSaveGameCloudKey>& out_keys)
{
  if (!IsCloudAvailable())
    return plSaveGameError::IoError;

  ISteamRemoteStorage* pStorage = SteamRemoteStorage();
  const plString sPrefix = CloudFilePrefix(sProject, profile);

  const plInt32 iCount = pStorage->GetFileCount();
  for (plInt32 i = 0; i < iCount; ++i)
  {
    plInt32 iSize = 0;
    const char* szName = pStorage->GetFileNameAndSize(i, &iSize);
    if (szName == nullptr)
      continue;

    plStringView sName = szName;
    if (!sName.StartsWith_NoCase(sPrefix) || !sName.EndsWith_NoCase(s_szCloudSuffix))
      continue;

    sName.Shrink(sPrefix.GetElementCount(), plStringUtils::GetStringElementCount(s_szCloudSuffix));

    plSaveGameCloudKey& key = out_keys.ExpandAndGetRef();
    key.m_sProject = sProject;
    key.m_Profile = profile;

    if (sName.IsEqual_NoCase(s_szCloudProfileSlot))
    {
      key.m_Slot = plUuid::MakeInvalid();
    }
    else if (!ParseUuidHex(sName, key.m_Slot))
    {
      out_keys.PopBack();
    }
  }

  return plSaveGameError::Success;
}
