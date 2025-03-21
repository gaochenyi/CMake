/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file Copyright.txt or https://cmake.org/licensing#kwsys for details.  */
#ifndef testConsoleBuf_hxx
#define testConsoleBuf_hxx

static wchar_t const cmdConsoleBufChild[] = L"testConsoleBufChild";

static wchar_t const BeforeInputEventName[] = L"BeforeInputEvent";
static wchar_t const AfterOutputEventName[] = L"AfterOutputEvent";

// यूनिकोड είναι здорово!
static wchar_t const UnicodeTestString[] =
  L"\u092F\u0942\u0928\u093F\u0915\u094B\u0921 "
  L"\u03B5\u03AF\u03BD\0\u03B1\u03B9 "
  L"\u0437\u0434\u043E\u0440\u043E\u0432\u043E!";

#endif
