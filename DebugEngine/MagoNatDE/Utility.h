/*
   Copyright (c) 2010 Aldo J. Nunez

   Licensed under the Apache License, Version 2.0.
   See the LICENSE text file for details.
*/

#pragma once


inline HRESULT GetLastHr()
{
    return HRESULT_FROM_WIN32( GetLastError() );
}


template <class T>
HRESULT MakeCComObject( RefPtr<T>& refPtr )
{
    HRESULT         hr = S_OK;
    CComObject<T>*  t = NULL;

    hr = CComObject<T>::CreateInstance( &t );
    if ( FAILED( hr ) )
        return hr;

    refPtr = t;
    return S_OK;
}


inline bool IsSymHandleNull( const MagoST::SymHandle& handle )
{
    MagoST::SymHandle   emptyHandle = { 0 };

    return memcmp( &handle, &emptyHandle, sizeof handle ) == 0;
}


template <typename T, typename Copy>
class ScopedArray
{
    T*          mArray;
    size_t      mLen;

public:
    ScopedArray( size_t length )
        :   mArray( new T[ length ] ),
            mLen( length )
    {
        if ( mArray == NULL )
            return;

        size_t  size = length * sizeof( T );
        memset( mArray, 0, size );
    }

    ~ScopedArray()
    {
        if ( mArray == NULL )
            return;

        for ( size_t i = 0; i < mLen; i++ )
        {
            Copy::destroy( &mArray[i] );
        }

        delete [] mArray;
    }

    size_t GetLength() const
    {
        return mLen;
    }

    T& operator[]( size_t i ) const
    {
        _ASSERT( i < mLen );
        return mArray[i];
    }

    T* Get() const
    {
        return mArray;
    }

    T* Detach()
    {
        T*  array = mArray;
        mArray = NULL;
        return array;
    }

private:
    ScopedArray( const ScopedArray& );
    ScopedArray& operator=( const ScopedArray& );
};


// maybe we can use perfect forwarding in C++0x to make a MakeCComObjectInit that can forward arguments to Init()


std::wstring BuildCommandLine( const wchar_t* exe, const wchar_t* args );

HRESULT GetProcessId( IDebugProcess2* proc, DWORD& id );
HRESULT GetProcessId( IDebugProgram2* proc, DWORD& id );


namespace Mago
{
    class Program;
    class Module;

    class ProgramCallback
    {
    public:
        virtual bool    AcceptProgram( Program* program ) = 0;
    };

    class ModuleCallback
    {
    public:
        virtual bool    AcceptModule( Module* module ) = 0;
    };
}


HRESULT Utf8To16( const char* u8Str, size_t u8StrLen, BSTR& u16Str );
HRESULT Utf16To8( const wchar_t* u16Str, size_t u16StrLen, char*& u8Str, size_t& u8StrLen );

bool ExactFileNameMatch( const char* pathA, size_t pathALen, const char* pathB, size_t pathBLen );
bool PartialFileNameMatch( const char* pathA, size_t pathALen, const char* pathB, size_t pathBLen );

namespace MagoEE
{
    union DataValue;
    class Type;
}

struct Real10;


void ConvertVariantToDataVal( const MagoST::Variant& var, MagoEE::DataValue& dataVal );

uint64_t ReadInt( const void* srcBuf, uint32_t bufOffset, size_t size, bool isSigned );
Real10 ReadFloat( const void* srcBuf, uint32_t bufOffset, MagoEE::Type* type );
HRESULT WriteInt( uint8_t* buffer, uint32_t bufSize, MagoEE::Type* type, uint64_t val );
HRESULT WriteFloat( uint8_t* buffer, uint32_t bufSize, MagoEE::Type* type, const Real10& val );
HRESULT FromRawValue( const void* srcBuf, MagoEE::Type* type, MagoEE::DataValue& value );

typedef bool (*EqualsFunc)( const void* leftBuf, const void* rightBuf );

bool EqualFloat32( const void* leftBuf, const void* rightBuf );
bool EqualFloat64( const void* leftBuf, const void* rightBuf );
bool EqualFloat80( const void* leftBuf, const void* rightBuf );
bool EqualComplex32( const void* leftBuf, const void* rightBuf );
bool EqualComplex64( const void* leftBuf, const void* rightBuf );
bool EqualComplex80( const void* leftBuf, const void* rightBuf );
EqualsFunc GetFloatingEqualsFunc( MagoEE::Type* type );

bool EqualFloat( size_t typeSize, const Real10& left, const Real10& right );
bool EqualValue( MagoEE::Type* type, const MagoEE::DataValue& left, const MagoEE::DataValue& right );

uint32_t HashOf( const void* buffer, uint32_t length );


class HeapCloser
{
public:
    void operator()( uint8_t* p )
    {
        HeapFree( GetProcessHeap(), 0, p );
    }
};

typedef HandlePtrBase2<uint8_t*, NULL, HeapCloser> HeapPtr;
