// melonDS Platform:: services that don't depend on host state: files, threads, sync,
// time, and stubs for features we don't support yet (local wifi, camera, AAC, addons).
// The host-state callbacks (Log, SignalStop, saves, RTC, mic) live in emu_host.cpp.

#include "frontend/melon_platform.h"

#include <Platform.h>

#include <sys/stat.h>

#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <thread>

#include "platform/system.h"

namespace melon_platform {

namespace {
std::string g_local_dir = ".";
}  // namespace

void set_local_dir(const std::string& dir) {
    g_local_dir = dir;
}

}  // namespace melon_platform

namespace melonDS::Platform {

namespace {

// melonDS's only worker thread today is the soft 3D renderer. UI is core 0 and the emu
// thread core 1 (emu_host.cpp), so workers share core 2 with the (higher-priority, light)
// audio thread.
constexpr int kWorkerCore = 2;

FILE* fp(FileHandle* f) {
    return reinterpret_cast<FILE*>(f);
}

bool exists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

// Same fopen mode mapping as melonDS's Qt frontend (Platform.cpp GetModeString).
std::string mode_string(FileMode mode, bool file_exists) {
    std::string m;
    if (mode & FileMode::Append)
        m += 'a';
    else if (!(mode & FileMode::Write))
        m += 'r';
    else if (mode & FileMode::NoCreate)
        m += 'r';
    else if ((mode & FileMode::Preserve) && file_exists)
        m += 'r';
    else
        m += 'w';
    if ((mode & FileMode::ReadWrite) == FileMode::ReadWrite)
        m += '+';
    if (!(mode & FileMode::Text))
        m += 'b';
    return m;
}

}  // namespace

std::string GetLocalFilePath(const std::string& filename) {
    if (!filename.empty() && (filename[0] == '/' || filename.find(':') != std::string::npos))
        return filename;  // absolute ("sdmc:/..." or "/...")
    return melon_platform::g_local_dir + "/" + filename;
}

FileHandle* OpenFile(const std::string& path, FileMode mode) {
    if ((mode & (FileMode::ReadWrite | FileMode::Append)) == FileMode::None)
        return nullptr;
    bool present = exists(path);
    if ((mode & FileMode::NoCreate) && !present)
        return nullptr;
    return reinterpret_cast<FileHandle*>(fopen(path.c_str(), mode_string(mode, present).c_str()));
}

FileHandle* OpenLocalFile(const std::string& path, FileMode mode) {
    return OpenFile(GetLocalFilePath(path), mode);
}

bool FileExists(const std::string& name) {
    return exists(name);
}

bool LocalFileExists(const std::string& name) {
    return exists(GetLocalFilePath(name));
}

bool CheckFileWritable(const std::string& filepath) {
    FileHandle* f = OpenFile(filepath, FileMode::Append);
    if (!f)
        return false;
    CloseFile(f);
    return true;
}

bool CheckLocalFileWritable(const std::string& filepath) {
    return CheckFileWritable(GetLocalFilePath(filepath));
}

bool CloseFile(FileHandle* file) {
    return fclose(fp(file)) == 0;
}

bool IsEndOfFile(FileHandle* file) {
    return feof(fp(file)) != 0;
}

bool FileReadLine(char* str, int count, FileHandle* file) {
    return fgets(str, count, fp(file)) != nullptr;
}

u64 FilePosition(FileHandle* file) {
    return u64(ftell(fp(file)));
}

bool FileSeek(FileHandle* file, s64 offset, FileSeekOrigin origin) {
    int whence = origin == FileSeekOrigin::Start ? SEEK_SET
               : origin == FileSeekOrigin::Current ? SEEK_CUR : SEEK_END;
    return fseek(fp(file), long(offset), whence) == 0;
}

void FileRewind(FileHandle* file) {
    rewind(fp(file));
}

u64 FileRead(void* data, u64 size, u64 count, FileHandle* file) {
    return fread(data, size, count, fp(file));
}

bool FileFlush(FileHandle* file) {
    return fflush(fp(file)) == 0;
}

u64 FileWrite(const void* data, u64 size, u64 count, FileHandle* file) {
    return fwrite(data, size, count, fp(file));
}

u64 FileWriteFormatted(FileHandle* file, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vfprintf(fp(file), fmt, ap);
    va_end(ap);
    return n < 0 ? 0 : u64(n);
}

u64 FileLength(FileHandle* file) {
    FILE* f = fp(file);
    long pos = ftell(f);
    if (pos < 0 || fseek(f, 0, SEEK_END) != 0)
        return 0;
    long len = ftell(f);
    fseek(f, pos, SEEK_SET);
    return len < 0 ? 0 : u64(len);
}

// --- threads / sync --------------------------------------------------------------

struct Thread {
    std::thread t;
};

Thread* Thread_Create(std::function<void()> func) {
    auto* th = new Thread;
    th->t = std::thread([f = std::move(func)]() {
        sys::set_current_thread_core(kWorkerCore);
        f();
    });
    return th;
}

void Thread_Free(Thread* thread) {
    // melonDS always Thread_Wait()s first. (libnx can't detach threads: detach() throws.)
    if (thread->t.joinable())
        thread->t.join();
    delete thread;
}

void Thread_Wait(Thread* thread) {
    if (thread->t.joinable())
        thread->t.join();
}

struct Semaphore {
    std::mutex m;
    std::condition_variable cv;
    int count = 0;
};

Semaphore* Semaphore_Create() {
    return new Semaphore;
}

void Semaphore_Free(Semaphore* sema) {
    delete sema;
}

void Semaphore_Reset(Semaphore* sema) {
    std::lock_guard<std::mutex> lk(sema->m);
    sema->count = 0;
}

void Semaphore_Wait(Semaphore* sema) {
    std::unique_lock<std::mutex> lk(sema->m);
    sema->cv.wait(lk, [sema] { return sema->count > 0; });
    --sema->count;
}

bool Semaphore_TryWait(Semaphore* sema, int timeout_ms) {
    std::unique_lock<std::mutex> lk(sema->m);
    if (timeout_ms > 0)
        sema->cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [sema] { return sema->count > 0; });
    if (sema->count <= 0)
        return false;
    --sema->count;
    return true;
}

void Semaphore_Post(Semaphore* sema, int count) {
    {
        std::lock_guard<std::mutex> lk(sema->m);
        sema->count += count;
    }
    if (count == 1)
        sema->cv.notify_one();
    else
        sema->cv.notify_all();
}

struct Mutex {
    std::mutex m;
};

Mutex* Mutex_Create() {
    return new Mutex;
}

void Mutex_Free(Mutex* mutex) {
    delete mutex;
}

void Mutex_Lock(Mutex* mutex) {
    mutex->m.lock();
}

void Mutex_Unlock(Mutex* mutex) {
    mutex->m.unlock();
}

bool Mutex_TryLock(Mutex* mutex) {
    return mutex->m.try_lock();
}

void Sleep(u64 usecs) {
    std::this_thread::sleep_for(std::chrono::microseconds(usecs));
}

u64 GetMSCount() {
    using namespace std::chrono;
    return u64(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

u64 GetUSCount() {
    using namespace std::chrono;
    return u64(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

// --- unsupported for now -----------------------------------------------------------

void WriteGBASave(const u8*, u32, u32, u32, void*) {}  // no GBA slot carts loaded

void MP_Begin(void*) {}
void MP_End(void*) {}
int MP_SendPacket(u8*, int, u64, void*) { return 0; }
int MP_RecvPacket(u8*, u64*, void*) { return 0; }
int MP_SendCmd(u8*, int, u64, void*) { return 0; }
int MP_SendReply(u8*, int, u64, u16, void*) { return 0; }
int MP_SendAck(u8*, int, u64, void*) { return 0; }
int MP_RecvHostPacket(u8*, u64*, void*) { return 0; }
u16 MP_RecvReplies(u8*, u64, u16, void*) { return 0; }

int Net_SendPacket(u8*, int, void*) { return 0; }
int Net_RecvPacket(u8*, void*) { return 0; }

void Camera_Start(int, void*) {}
void Camera_Stop(int, void*) {}
void Camera_CaptureFrame(int, u32*, int, int, bool, void*) {}

AACDecoder* AAC_Init() { return nullptr; }
void AAC_DeInit(AACDecoder*) {}
bool AAC_Configure(AACDecoder*, int, int) { return false; }
bool AAC_DecodeFrame(AACDecoder*, const void*, int, void*, int) { return false; }

bool Addon_KeyDown(KeyType, void*) { return false; }
void Addon_RumbleStart(u32, void*) {}
void Addon_RumbleStop(void*) {}
float Addon_MotionQuery(MotionQueryType, void*) { return 0.0f; }

DynamicLibrary* DynamicLibrary_Load(const char*) { return nullptr; }
void DynamicLibrary_Unload(DynamicLibrary*) {}
void* DynamicLibrary_LoadFunction(DynamicLibrary*, const char*) { return nullptr; }

}  // namespace melonDS::Platform
