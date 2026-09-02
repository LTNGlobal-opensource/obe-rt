//#*#*#*#*#*#*#*#*#*#*#*#*#*#*#*#*#* MxExamplesCommon.h *#*#*#*#*#*#* (C) 2014-2015 DekTec
//

#ifndef  __MXEXAMPLESCOMMON_H
#define  __MXEXAMPLESCOMMON_H

//.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.- Include files -.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-
#include "DTAPI.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string>
#include <cmath>

#ifdef _WIN32
    // Windows specific includes
    #include <windows.h>
    #include <stddef.h>     // For (u)intptr_t
#else
    // Linux specific includes
    #include <sys/types.h>
    #include <unistd.h>
    #include <errno.h>
    #include <stdint.h>     // For (u)intptr_t
    #include <sys/time.h>
#endif


// All code resides in MxExamples namespace

//.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.- MX_ASSERT -.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-
//
#ifndef _DEBUG
#define MX_ASSERT(Expr)  ((void)0)
#else // _DEBUG
#ifdef _WIN32
#define MX_ASSERT(Expr)  _ASSERTE(Expr)
#else // _WIN32
#define MX_ASSERT(Expr)  if(!(Expr)) printf("Assertion failed @ %s, Line %d: (%s)\n", \
                                                               __FILE__, __LINE__, #Expr);
#endif // _WIN32
#endif // _DEBUG

//-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.- class MxDemoExc -.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.
//
class MxDemoExc
{
    // Operations
public:
    const char*  ErrorStr() const { return m_ErrorString.c_str(); }
    
    // Data / Attributes
public:
protected:
    std::string  m_ErrorString;    // Name of the function
    DTAPI_RESULT  m_ErrorCode;     // DTAPI result code
    
    // Constructor / Destructor
public:
    MxDemoExc(DTAPI_RESULT  ErrorCode, const char*  pFile, int  Line, 
                                                                   const char*  pMsg, ...) 
    {
        int  Len = 0;
        char  Buffer[1024];
        Len = snprintf(Buffer, sizeof(Buffer), "%s(%d): ", pFile, Line);
        if (Len < 0)
            Len = 0;
        if (Len > (int)sizeof(Buffer) - 1)
            Len = (int)sizeof(Buffer) - 1;
        if (Len >= 0)
        {
            // Add message and inserts to the debug string
            va_list  VaArgs;
            va_start(VaArgs, pMsg);
            int MsgLen = vsnprintf(&Buffer[Len], sizeof(Buffer) - Len, pMsg, VaArgs);
            va_end(VaArgs);
            if (MsgLen > 0)
                Len += MsgLen;
            if (Len > (int)sizeof(Buffer) - 1)
                Len = (int)sizeof(Buffer) - 1;
        }
        // Append error code
        if (ErrorCode != DTAPI_OK) {
            int ErrLen = snprintf(&Buffer[Len], sizeof(Buffer) - Len, " (ERROR=%s)", ::DtapiResult2Str(ErrorCode));
            if (ErrLen > 0)
                Len += ErrLen;
            if (Len > (int)sizeof(Buffer) - 1)
                Len = (int)sizeof(Buffer) - 1;
        }
        Buffer[Len++] = '\0';
        m_ErrorString = Buffer;
        m_ErrorCode = ErrorCode;
    }
};
#define MX_THROW_EXC(ErrorCode, pMsg, ...)                                  \
do                                                                          \
{                                                                           \
    throw MxDemoExc(ErrorCode, __FILE__, __LINE__, pMsg, ##__VA_ARGS__);    \
} while (0)
#define MX_THROW_EXC_NO_ERR(pMsg, ...)  MX_THROW_EXC(DTAPI_OK, pMsg, ##__VA_ARGS__)

//.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.- class MxDemoMatrixBase -.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.
//
// Base class for matrix examples implementations
//
class MxDemoMatrixBase
{
    // Operations
public:
    virtual bool  Init(DtDevice&  TheCard) = 0;
    virtual void  Reset() { m_TheMatrix.Reset(); }
    virtual bool  Start()
    {
        DTAPI_RESULT  dr = m_TheMatrix.Start();
        if (dr != DTAPI_OK)
            MX_THROW_EXC(dr, "Failed to start the matrix");
        return true;
    }
    virtual void  Stop() { m_TheMatrix.Stop(); }
    virtual void  PrintProfilingInfo() { m_TheMatrix.PrintProfilingInfo(); }
protected:
    
    // Data / Attributes
public:
protected:
    DtMxProcess  m_TheMatrix;   // The actual matrix process
    
    // Constructor / Destructor
public:
    MxDemoMatrixBase() {}
    virtual ~MxDemoMatrixBase() { Stop(); }
};

//.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.- Color -.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-
//
// Represent the YUV value for a color
//
struct  Color
{
    unsigned char  m_Y, m_U, m_V;
    Color(unsigned char Y, unsigned char  U, unsigned char  V) 
                                                            : m_Y(Y), m_U(U), m_V(V) {}

    unsigned short Pixel1() { return ((unsigned short)m_Y)<<8 | ((unsigned short)m_U); }
    unsigned short Pixel2() { return ((unsigned short)m_Y)<<8 | ((unsigned short)m_V); }
};
// Colors
static const  Color COL_BLACK   = Color(0x10, 0x80, 0x80);
static const  Color COL_WHITE   = Color(0xEB, 0x80, 0x80);
static const  Color COL_RED     = Color(0x51, 0x59, 0xF0);
static const  Color COL_BLUE    = Color(0x29, 0xF0, 0x6E);
static const  Color COL_ORANGE  = Color(0x9C, 0x2F, 0xB9);


//-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.- MxPoint -.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.
//
// Represent a MxPoint
//
struct MxPoint
{
    int  m_X, m_Y;

    MxPoint operator + (const MxPoint&  Add)
    {
        MxPoint  P(*this);
        P.m_X += Add.m_X;  P.m_Y += Add.m_Y;
        return P;
    }
    MxPoint& operator += (const MxPoint&  Add)
    {
        *this = *this + Add;
        return *this;
    }
    MxPoint operator - (const MxPoint&  Sub)
    {
        MxPoint  P(*this);
        P.m_X -= Sub.m_X;  P.m_Y -= Sub.m_Y;
        return P;
    }
    MxPoint& operator -= (const MxPoint&  Sub)
    {
        *this = *this - Sub;
        return *this;
    }

    void  Offset(int  x, int y) { *this += MxPoint(x,y); }
    void  Offset(const  MxPoint&  P) { Offset(P.m_X, P.m_Y); }

    MxPoint() : m_X(0), m_Y(0) {}
    MxPoint(int  x, int  y) : m_X(x), m_Y(y) {}
};

//.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.- MxInvalidBitIdxExc -.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.
//
// Exception class that is thrown when you try to read more bits than available.
class MxInvalidBitIdxExc : public std::exception
{
    virtual const char*  what() const throw()
    {
        return "InvalidBitIdxExc: Read outside the buffer";
    }
};

//-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.- MxBitPtr -.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-
//
// Bit-stream pointer.
// Used to read 'n' bits from a buffer. If you try to read more bits then there are
// available in the given buffer an InvalidBitIdxExc exception will be thrown.
//
class MxBitPtr 
{
public:
    MxBitPtr(unsigned char* Start, unsigned char* End) :
        m_Start(Start),
        m_End(End),
        m_Byte(Start),
        m_BitPos(8)
    {}

    int  BitNum() const { return (BytePos()*8) + (8-m_BitPos); }
    int  BytePos() const { return int(m_Byte-m_Start); }

    //-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-. MxBitPtr::GetBits -.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-
    //
    // Read n bits from the internal buffer. If there are not enough bits available an
    // InvalidBitIdxExc exception will be thrown.
    //
    __int64  GetBits(int n)
    {
        MX_ASSERT(n <= 64);
        if (m_Byte == m_End)
            throw MxInvalidBitIdxExc();

        if (n < m_BitPos)
            return (*m_Byte>>(m_BitPos-=n)) & ((1<<n)-1);

        __int64  Result = *m_Byte++ & ((1<<m_BitPos)-1);
        n -= m_BitPos;
        while (n >= 8)
        {
            if (m_Byte == m_End)
                throw MxInvalidBitIdxExc();

            Result = (Result<<8) | *m_Byte++;
            n-=8;
        }
        m_BitPos = 8-n;
        
        if (m_Byte==m_End && n>0)
            throw MxInvalidBitIdxExc();

        if (n > 0)
            return (Result<<n) | ((*m_Byte>>m_BitPos) & ((1<<n)-1));

        return Result;
    }

    //-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.- SetBits -.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.
    //
    // Write n bits to the internal buffer. If there are not enough bits available an
    // InvalidBitIdxExc exception will be thrown.
    //
    void  SetBits(int  n, __int64  Value)
    {
        MX_ASSERT(n <= 64);
        if (m_Byte == m_End)
            throw MxInvalidBitIdxExc();

        unsigned int  S=0, M=0;
        if (n < m_BitPos)
        {
            // Conmpute shift and mask
            S = (m_BitPos-=n); M = ((1<<n)-1);
            *m_Byte &= ~(M<<S);
            *m_Byte |= (Value&M)<<S;
            return;
        }
        
        // Compute mask
        M = (1<<m_BitPos)-1;
        *m_Byte &= ~M;
        *m_Byte++ |= ((Value>>(n-m_BitPos))&M);
        
        n-=m_BitPos;
        while(n >= 8)
        {
            if (m_Byte == m_End)
                throw MxInvalidBitIdxExc();

            *m_Byte++ = ((Value>>(n-8)) & 0xFF);
            n-=8;
        }
        m_BitPos = 8-n;

        if (n > 0)
        {
            S = m_BitPos; M = ((1<<n)-1);
            *m_Byte &= ~(M<<S);
            *m_Byte |= (Value&M)<<S;
        }
    }

private:
    const unsigned char*  m_Start;  // Start position of buffer
    const unsigned char*  m_End;    // Pointer to last byte + 1
    unsigned char*  m_Byte;         // Byte pointer
    int  m_BitPos;                  // 8=MSB .. 1=LSB
};

class MxBitPtrNative
{
public:
    MxBitPtrNative(unsigned char* Start, unsigned char* End) :
        m_Byte(Start),
        m_End(End),
        m_BitPos(0)
    {}

    //-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-. MxBitPtr::GetBits -.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-
    //
    // Read n bits from the internal buffer. If there are not enough bits available an
    // InvalidBitIdxExc exception will be thrown.
    //
    int  GetBits(int n)
    {
        MX_ASSERT(n <= 32);
        if (m_Byte == m_End)
            throw MxInvalidBitIdxExc();

        int  Result = 0;
        if ((m_BitPos+n)<8)
        {
            Result = (*m_Byte>>m_BitPos) & ((1<<n)-1);
            m_BitPos += n;
            return Result;
        }

        int  S = 0;
        Result = (*m_Byte++ >> m_BitPos) & ((1<<(8-m_BitPos))-1);
        n -= (8-m_BitPos);
        S = (8-m_BitPos);
        while (n >= 8)
        {
            if (m_Byte == m_End)
                throw MxInvalidBitIdxExc();

            Result = (*m_Byte++ << S) | Result;
            n-=8; S+=8;
        }
        m_BitPos = n;
        
        if (m_Byte==m_End && n>0)
            throw MxInvalidBitIdxExc();

        if (n > 0)
            return (((*m_Byte)&((1<<n)-1))<<S) | Result;

        return Result;
    }

    //-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.- SetBits -.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.
    //
    // Write n bits to the internal buffer. If there are not enough bits available an
    // InvalidBitIdxExc exception will be thrown.
    //
    void  SetBits(int  n, int  Value)
    {
        MX_ASSERT(n <= 32);
        if (m_Byte == m_End)
            throw MxInvalidBitIdxExc();

        unsigned int  S=0, M=0;
        if ((m_BitPos+n)<8)
        {
            // Compute shift and mask
            S = m_BitPos; M = ((1<<n)-1);
            *m_Byte &= ~(M<<S);
            *m_Byte |= (Value&M)<<S;
            m_BitPos += n;
            return;
        }
        
        // Compute shift and mask
        S = m_BitPos; M = (1<<(8-m_BitPos))-1;
        *m_Byte &= ~(M<<S);
        *m_Byte++ |= ((Value&M)<<S);
        
        n-=(8-m_BitPos);
        S = (8-m_BitPos);
        while(n >= 8)
        {
            if (m_Byte == m_End)
                throw MxInvalidBitIdxExc();

            *m_Byte++ = (Value>>S) & 0xFF;
            n-=8; S+=8;
        }
        m_BitPos = n;
        
        if (m_Byte==m_End && n>0)
            throw MxInvalidBitIdxExc();
        
        if (n > 0)
        {
            M = ((1<<n)-1);
            *m_Byte &= ~M;
            *m_Byte |= (Value>>S)&M;
        }
    }

private:
    unsigned char*  m_Byte;         // Byte pointer
    unsigned char*  m_End;          // Pointer to last byte + 1
    int  m_BitPos;                  // 7=MSB .. 0=LSB
};

//=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+
//=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+ Crossplatform Helpers +=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+
//=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+

//.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.- MxUtility -.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-
//
class  MxUtility
{
public:
    static void  FreeAligned(void* MPtr)
    {
        void*  Ptr = ((void**)MPtr)[-1];
        free(Ptr);
    }

    static void*  MallocAligned(int Align, int Size)
    {
        unsigned char*  Ptr = (unsigned char*) malloc(Size + Align - 1 + sizeof(void*));
        if (Ptr == NULL)
            return NULL;
        unsigned char*  MPtr = Ptr + sizeof(void*);
        MPtr = (unsigned char*)(((uintptr_t)MPtr + Align - 1) & ~(Align - 1));
        ((void **)MPtr)[-1] = Ptr;
        return MPtr;
    }

private:
    MxUtility() {}
};
#define MX_UTIL  MxUtility

//.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.- IMxThread -.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-
typedef void  (*pThreadFunc)(void*);
class IMxThread
{
public:
    //-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.- ThreadPrioLevel -.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.
    // Defines the thread priority levels
    enum ThreadPrioLevel {
        THREAD_PRIO_LOWEST,
        THREAD_PRIO_BELOW_NORMAL,
        THREAD_PRIO_NORMAL,
        THREAD_PRIO_ABOVE_NORMAL,
        THREAD_PRIO_HIGHEST
    };

public:
    virtual bool  Create(pThreadFunc ThreadFunc, void* pArg) = 0;
    virtual bool  SetPriority(ThreadPrioLevel Prio) = 0;
    virtual bool  WaitFinished() = 0;
    virtual void  Close() = 0;
};

//.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.- IMxCritSec -.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.
class IMxCritSec
{
public:
    virtual bool  Init() = 0;
    virtual bool  Lock() = 0;
    virtual bool  Unlock() = 0;
    virtual void  Close() = 0;
};

//.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.- MxAutoCritSec -.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-
class MxAutoCritSec
{
    // Operations
public:
    inline void  Lock() 
    {
        if (m_IsLocked)
            return;
        MX_ASSERT(m_pCritSec != NULL);
        m_pCritSec->Lock();
        m_IsLocked = true;
    }
    inline void  Unlock()
    {
        if (!m_IsLocked)
            return;
        m_pCritSec->Unlock();
        m_IsLocked = false;
    }

    // Data / attributes
public:
private:
    bool  m_IsLocked;
    IMxCritSec*  m_pCritSec;

    // Constructor / Destructor
public:
    MxAutoCritSec(IMxCritSec*  pCritSec,  bool  LockNow=true) 
                                                 : m_IsLocked(false), m_pCritSec(pCritSec)
    {
        MX_ASSERT(pCritSec != NULL);
        if (LockNow)
            Lock();
    }
    virtual ~MxAutoCritSec() { Unlock(); }

};

//-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.- IMxEvent -.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-
class IMxEvent
{
public:
    virtual bool  Init(bool ManualReset=false) = 0;
    virtual bool  Set() = 0;
    virtual bool  Reset() = 0;
    virtual bool  Wait() = 0;
    virtual bool  WaitTimeout(unsigned int Timeout) = 0;
    virtual void  Close() = 0;
};

#ifdef _WIN32
//-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.- MxThreadWindows -.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.
#define MxThread  MxThreadWindows
class MxThreadWindows : public IMxThread
{
public:
    MxThreadWindows() : m_Running(false), m_hThread(NULL) {}
    virtual ~MxThreadWindows() { Close(); }

    // IMxThread overrides
    virtual bool  Create(pThreadFunc ThreadFunc, void* pArg)
    {
        unsigned int ThreadId=-1;

        if (ThreadFunc == NULL)
            return false;
        if (m_hThread != NULL)
            return false;

        m_Running = false;

        // Store parameters
        m_pThreadFunc = ThreadFunc;
        m_pArg = pArg;

        // Start thread
        m_hThread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)StaticThreadFunc,
                                                     (LPVOID)this, 0, (LPDWORD)&ThreadId);

        // Check if thread was created
        if (m_hThread != NULL)
            m_Running = true;

        return m_Running;
    }

    virtual bool  SetPriority(ThreadPrioLevel Prio)
    {
        // Check that thread is running
        if (m_hThread == NULL)
            return false;

        DWORD WinPrio;
        switch(Prio)
        {
        case    THREAD_PRIO_LOWEST:       WinPrio = THREAD_PRIORITY_LOWEST; break;
        case    THREAD_PRIO_BELOW_NORMAL: WinPrio = THREAD_PRIORITY_BELOW_NORMAL; break;
        case    THREAD_PRIO_NORMAL:       WinPrio = THREAD_PRIORITY_NORMAL; break;
        case    THREAD_PRIO_ABOVE_NORMAL: WinPrio = THREAD_PRIORITY_ABOVE_NORMAL; break;
        case    THREAD_PRIO_HIGHEST:      WinPrio = THREAD_PRIORITY_HIGHEST; break;
        default: 
            return false;
        }
    
        // Set thread priority 
        ::SetThreadPriority(m_hThread, WinPrio);
        return DTAPI_OK;
    }
    virtual bool  WaitFinished()
    {
        // Check that thread is running
        if (m_hThread == NULL)
            return false;

        // Only wait if still running (not waited before)
        if (m_Running) 
        {
            // Wait for it to stop
            ::WaitForSingleObject(m_hThread, INFINITE);
            // Not running anymore
            m_Running = false;
        }
        return DTAPI_OK;
    }
    virtual void  Close()
    {
        // Make sure thread is stopped
        WaitFinished();

        // Close thread handle
        if (m_hThread != NULL)
        {
            CloseHandle(m_hThread);
            m_hThread = NULL;
        }
    }

private:
    static DWORD  StaticThreadFunc(void* pArg)
    {
        MxThreadWindows*  pThread = (MxThreadWindows*)pArg;
        // Call user thread func
        pThread->m_pThreadFunc(pThread->m_pArg);
        return 0;
    }

    bool  m_Running;
    HANDLE  m_hThread;
    pThreadFunc  m_pThreadFunc;
    void*  m_pArg;
};

//-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.- MxCritSecWindows -.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-
#define MxCritSec MxCritSecWindows
class MxCritSecWindows : public IMxCritSec
{
public:
    MxCritSecWindows() : m_Initialized(false) {}
    virtual ~MxCritSecWindows() { Close(); }

    // IMxCritSec overrides
    virtual bool  Init()
    {
        if (!m_Initialized) 
        {
            // Initialize critical section
            InitializeCriticalSection(&m_CritSec);
            // Initialize succeeded
            m_Initialized = true;
        }
        return m_Initialized;
    }
    virtual bool  Lock()
    {
        if (!m_Initialized)
            return false;
        EnterCriticalSection(&m_CritSec);
        return true;
    }
    virtual bool  Unlock()
    {
         if (!m_Initialized)
            return false;
        LeaveCriticalSection(&m_CritSec);
        return true;
    }
    virtual void  Close()
    {
        if (m_Initialized)
        {
            // Clean up resources
            DeleteCriticalSection(&m_CritSec);
            // Not initialized anymore
            m_Initialized = false;
        }
    }

private:
    bool  m_Initialized;
    CRITICAL_SECTION  m_CritSec;
};

//.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.- MxEventWindows -.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.
#define MxEvent  MxEventWindows
class MxEventWindows : public IMxEvent
{
public:
    MxEventWindows() : m_hEvent(NULL) {}
    virtual ~MxEventWindows() { Close(); }

    // IMxEvent overrides
    virtual bool  Init(bool ManualReset=false)
    {
        if (m_hEvent != NULL)
            return false;
        // Create the event
        m_hEvent = CreateEvent(NULL, ManualReset, false, NULL);
        if (m_hEvent == NULL)
            return false;
        return true;
    }
    virtual bool  Reset()
    {
        if (m_hEvent == NULL)
            return false;
        BOOL  Result = ResetEvent(m_hEvent);
        if (Result == 0)
            return false;
        return true;
    }
    virtual bool  Set()
    {
        if (m_hEvent == NULL)
            return false;
        BOOL  Result = SetEvent(m_hEvent);
        if (Result == 0)
            return false;
        return true;
    }
    virtual bool  Wait()
    {
        if (m_hEvent == NULL)
            return false;
        DWORD  Result = WaitForSingleObject(m_hEvent, INFINITE);
        if (Result!=WAIT_OBJECT_0 && Result!=WAIT_ABANDONED)
            return false;
        return true;
    }
    virtual bool  WaitTimeout(unsigned int Timeout)
    {
        if (m_hEvent == NULL)
            return false;
        DWORD  Result = WaitForSingleObject(m_hEvent, Timeout);
        if (Result!=WAIT_OBJECT_0 && Result!=WAIT_ABANDONED)
            return false;
        return true;
    }
    virtual void  Close()
    {
        if (m_hEvent != NULL) 
        {
            CloseHandle(m_hEvent);
            m_hEvent = NULL;
        }
    }

private:
    HANDLE  m_hEvent;
};

#else

//.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.- MxThreadLinux -.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-
#define MxThread  MxThreadLinux
class MxThreadLinux : public IMxThread
{
public:
    MxThreadLinux() : m_Created(false), m_Running(false) {}
    virtual ~MxThreadLinux() { Close(); }

    // IMpThread overrides
    virtual bool  Create(pThreadFunc ThreadFunc, void* pArg)
    {
        if (ThreadFunc == NULL)
            return false;
        if (m_Created)
            return false;

        m_Running = false;

        // Store parameters
        m_pThreadFunc = ThreadFunc;
        m_pArg = pArg;

        // Start thread
        if (pthread_create(&m_Thread, NULL, StaticThreadFunc, (void*)this) == 0)
        {
            m_Created = true;
            m_Running = true;
        }
        return m_Running;
    }
    virtual bool  SetPriority(ThreadPrioLevel Prio)
    {
        if (!m_Created)
            return false;

        int  LinPrio;
        switch(Prio)
        {
        case    THREAD_PRIO_LOWEST:       LinPrio = -2; break;
        case    THREAD_PRIO_BELOW_NORMAL: LinPrio = -1; break;
        case    THREAD_PRIO_NORMAL:       LinPrio =  0; break;
        case    THREAD_PRIO_ABOVE_NORMAL: LinPrio =  1; break;
        case    THREAD_PRIO_HIGHEST:      LinPrio =  2; break;
        default: 
            return false;
        }

        // Change priority
        pthread_setschedprio(m_Thread, LinPrio);
        return true;
    }
    virtual bool  WaitFinished()
    {
        if (!m_Created)
            return false;

        // Check that thread is running
        if (m_Running)
        {
            // Wait for it to stop
            pthread_join(m_Thread, NULL);
            m_Running = false;
        }
        return true;
    }
    virtual void  Close()
    {
         // Make sure thread is stopped
        WaitFinished();    
        m_Created = false;

        // Nothing to clean up
        //
    }
    
private:
    static void*  StaticThreadFunc(void* pArg)
    {
        MxThreadLinux*  pThread = (MxThreadLinux*)pArg;
        // Call user thread func
        pThread->m_pThreadFunc(pThread->m_pArg);
        pthread_exit(NULL);
    }

    pthread_t  m_Thread;
    pThreadFunc  m_pThreadFunc;
    void*  m_pArg;
    bool  m_Created;
    bool  m_Running;
};

//-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.- MxCritSecLinux -.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-
#define MxCritSec MxCritSecLinux
class MxCritSecLinux : public IMxCritSec
{
public:
    MxCritSecLinux() : m_Initialized(false) {}
    virtual ~MxCritSecLinux() { Close(); }

    // IMxCritSec overrides
    virtual bool  Init()
    {
        if (m_Initialized)
            return false;

        pthread_mutexattr_t  Attr;
        if (pthread_mutexattr_init(&Attr) != 0)
            return false;
        if (pthread_mutexattr_settype(&Attr, PTHREAD_MUTEX_RECURSIVE))
            return false;

        // Initialize the mutex
        int  Result = pthread_mutex_init(&m_Mutex, &Attr);
        // Check if mutex init was successful
        if (Result == 0)
            m_Initialized = true;

        return m_Initialized;
    }
    virtual bool  Lock()
    {
        if (!m_Initialized)
            return false;
        int  Result = pthread_mutex_lock(&m_Mutex);
        if (Result != 0)
            return false;
        return true;
    }
    virtual bool  Unlock()
    {
        if (!m_Initialized)
            return false;
        int  Result = pthread_mutex_unlock(&m_Mutex);
        if (Result != 0)
            return false;
        return true;
    }
    virtual void  Close()
    {
        if (m_Initialized)
        {
            // Clean up resources
            pthread_mutex_destroy(&m_Mutex);
            // Not initialized anymore
            m_Initialized = false;
        }
    }

private:
    bool  m_Initialized;
    pthread_mutex_t  m_Mutex;
};

//.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.- MxEventLinux -.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.
#define MxEvent  MxEventLinux
class MxEventLinux : public IMxEvent
{
public:
    MxEventLinux() : m_Initialized(false), m_Event(false)  {}
    virtual ~MxEventLinux() { Close(); }

    // IMxEvent overrides
    virtual bool  Init(bool ManualReset=false)
    {
        int  Result;
        pthread_mutexattr_t  Attr;

        if (m_Initialized)
            return false;
        // Initialize mutex attributes
        if (pthread_mutexattr_init(&Attr) != 0)
            return false;

        // Initialize the mutex
        Result = pthread_mutex_init(&m_Mutex, &Attr);
        // Check if mutex init failed
        if (Result != 0)
            return false;

        // Initialize cond var
        Result = pthread_cond_init(&m_Cond, NULL);
        if (Result != 0)
        {
            pthread_mutex_destroy(&m_Mutex);
            return false;
        }
        
        // No event occurred yet
        m_Event = false;
        m_ManualReset = ManualReset;
        // Succesfully initialized
        m_Initialized = true;
        
        return true;
    }
    virtual bool  Reset()
    {
        int  Result;
        if (!m_Initialized) 
            return false;
        
        // Lock mutex
        Result = pthread_mutex_lock(&m_Mutex);
        if (Result != 0)
            return false;
    
        m_Event = false;
    
        // Unlock the corresponding mutex
        Result = pthread_mutex_unlock(&m_Mutex);
        if (Result != 0)
            return false;
        return true;    
    }
    virtual bool  Set()
    {
        int  Result;
        if (!m_Initialized) 
            return false;
        
        // Lock the corresponding mutex
        Result = pthread_mutex_lock(&m_Mutex);
        if (Result != 0)
            return false;

        // Set the event var
        m_Event = true;

        // Signal the cond var
        pthread_cond_broadcast(&m_Cond);

        // Unlock the corresponding mutex
        Result = pthread_mutex_unlock(&m_Mutex);
        if (Result != 0)
            return false;
        return true;
        
    }
    virtual bool  Wait()
    {
        int  Result;
        if (!m_Initialized) 
            return false;

        // Lock mutex
        Result = pthread_mutex_lock(&m_Mutex);
        if (Result != 0)
            return false;
    
        // pthread_cond_wait unlocks the mutex before blocking on the cond var
        while (!m_Event)
            pthread_cond_wait(&m_Cond, &m_Mutex);
        // pthread_cond_timedwait locks the mutex before returning

        // Automatic reset event?
        if (!m_ManualReset)
            m_Event = false;

        // Unlock mutex
        Result = pthread_mutex_unlock(&m_Mutex);
        if (Result != 0)
            return false;
        return true;
    }
    virtual bool  WaitTimeout(unsigned int Timeout)
    {
        struct timespec  EndTime;
        struct timeval  CurrentTime;
        struct timezone  TimeZone;
        long long  TimeOutNs;
        int  Result;
        bool  Event = false;

        if (!m_Initialized) 
            return false;

        // Get current time
        gettimeofday(&CurrentTime, &TimeZone);

        // Calculate timeout in ns
        TimeOutNs = ((long long)CurrentTime.tv_usec*1000) + ((long long)Timeout*1000000);

        // Construct EndTime
        EndTime.tv_sec = CurrentTime.tv_sec + (TimeOutNs/1000000000);
        EndTime.tv_nsec = TimeOutNs%1000000000;

        // Lock mutex
        Result = pthread_mutex_lock(&m_Mutex);
        if (Result != 0)
            return false;

        // Check if we should block
        if (!m_Event) 
        {
            // pthread_cond_timedwait unlocks the mutex before blocking on the cond var
            pthread_cond_timedwait(&m_Cond, &m_Mutex, &EndTime);
            // pthread_cond_timedwait locks the mutex before returning
        }

        // Store event occurred flag
        Event = m_Event;

        // Automatic reset event?
        if (!m_ManualReset)
            m_Event = false;
    
        // Unlock mutex
        Result = pthread_mutex_unlock(&m_Mutex);
        if (Result != 0)
            return false;

        return Event ? true : false;
    }
    virtual void  Close()
    {
        if (m_Initialized)
        {
            // Clean up resources
            pthread_cond_destroy(&m_Cond);
            pthread_mutex_destroy(&m_Mutex);
            // Not initialized anymore
            m_Initialized = false;
        }
    }

private:
    bool  m_Initialized;
    pthread_cond_t  m_Cond;
    pthread_mutex_t  m_Mutex;
    bool  m_Event;
    bool  m_ManualReset;
};

#endif

//=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+
//=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+ Utility functions +=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+
//=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+

const char*  FrameStatusToString(DtMxFrameStatus  Status)
{
    switch (Status)
    {
    case  DT_FRMSTATUS_OK:                  return "OK";
    case  DT_FRMSTATUS_SKIPPED:             return "SKIPPED";
    case  DT_FRMSTATUS_DISABLED:            return "DISABLED";
    case  DT_FRMSTATUS_DUPLICATE:           return "DUPLICATE";
    case  DT_FRMSTATUS_LATE:                return "LATE";
    case  DT_FRMSTATUS_NO_SIGNAL:           return "NO_SIGNAL";
    case  DT_FRMSTATUS_WRONG_VIDSTD:        return "WRONG_VIDSTD";
    case  DT_FRMSTATUS_DEV_DISCONNECTED:    return "DEV_DISCONNECTED";
    case  DT_FRMSTATUS_ERROR_INTERNAL:      return "ERROR_INTERNAL";
    }
    return "???";
}

#endif // #ifndef  __MXEXAMPLESCOMMON_H
