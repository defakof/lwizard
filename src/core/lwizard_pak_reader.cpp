#include "core/lwizard_pak_reader.h"

#include <bg3rustpaklib.h>

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace {

class PakHandle
{
public:
  explicit PakHandle(const QString& pakPath) : m_pak(bg3pak_open(pakPath.toUtf8().constData())) {}
  ~PakHandle()
  {
    if (m_pak)
      bg3pak_close(m_pak);
  }

  PakHandle(const PakHandle&)            = delete;
  PakHandle& operator=(const PakHandle&) = delete;

  explicit operator bool() const
  {
    return m_pak != nullptr;
  }
  Bg3Pak* get() const
  {
    return m_pak;
  }

private:
  Bg3Pak* m_pak = nullptr;
};

class BytesHandle
{
public:
  explicit BytesHandle(Bg3Bytes* bytes) : m_bytes(bytes) {}
  ~BytesHandle()
  {
    if (m_bytes)
      bg3_free_bytes(m_bytes);
  }

  BytesHandle(const BytesHandle&)            = delete;
  BytesHandle& operator=(const BytesHandle&) = delete;

  explicit operator bool() const
  {
    return m_bytes != nullptr;
  }
  const Bg3Bytes* get() const
  {
    return m_bytes;
  }

private:
  Bg3Bytes* m_bytes = nullptr;
};

QByteArray toByteArray(const BytesHandle& bytes)
{
  if (!bytes)
    return {};

  return QByteArray(reinterpret_cast<const char*>(bytes.get()->data),
                    static_cast<qsizetype>(bytes.get()->len));
}

} // namespace

namespace LWizardPakReader {

bool hasLocalization(const QString& pakPath, const QString& language)
{
  const PakHandle pak(pakPath);
  if (!pak)
    return false;
  return bg3pak_has_localization(pak.get(), language.toUtf8().constData());
}

QStringList listFiles(const QString& pakPath)
{
  const PakHandle pak(pakPath);
  if (!pak)
    return {};

  struct Ctx
  {
    QStringList names;
  };
  Ctx ctx;
  bg3pak_for_each_file(
      pak.get(),
      [](const char* name, void* ud) {
        static_cast<Ctx*>(ud)->names.append(QString::fromUtf8(name));
      },
      &ctx);

  return ctx.names;
}

QByteArray readFile(const QString& pakPath, const QString& entryPath)
{
  const PakHandle pak(pakPath);
  if (!pak)
    return {};

  const BytesHandle bytes(bg3pak_read_file(pak.get(), entryPath.toUtf8().constData()));
  return toByteArray(bytes);
}

QByteArray locaBytesToJsonCompressed(const QByteArray& locaBytes)
{
  if (locaBytes.isEmpty())
    return {};

  const BytesHandle out(
      bg3loca_to_json_compressed(reinterpret_cast<const uint8_t*>(locaBytes.constData()),
                                 static_cast<size_t>(locaBytes.size())));
  return toByteArray(out);
}

} // namespace LWizardPakReader
