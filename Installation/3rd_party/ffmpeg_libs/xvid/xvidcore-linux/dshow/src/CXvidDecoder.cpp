/*****************************************************************************
 *
 *  XVID MPEG-4 VIDEO CODEC
 *  - XviD Decoder part of the DShow Filter  -
 *
 *  Copyright(C) 2002-2004 Peter Ross <pross@xvid.org>
 *
 *  This program is free software ; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation ; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY ; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program ; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 *
 * $Id: CXvidDecoder.cpp,v 1.16.4.1 2009/05/28 15:52:34 Isibaar Exp $
 *
 ****************************************************************************/

/****************************************************************************
 *
 * 2003/12/11 - added some additional options, mainly to make the deblocking
 *              code from xvidcore available. Most of the new code is taken
 *              from Nic's dshow filter, (C) Nic, http://nic.dnsalias.com
 *
 ****************************************************************************/

 /* 
	this requires the directx sdk
	place these paths at the top of the Tools|Options|Directories list

	headers:
	C:\DX90SDK\Include
	C:\DX90SDK\Samples\C++\DirectShow\BaseClasses
	
	C:\DX90SDK\Samples\C++\DirectShow\BaseClasses\Release
	C:\DX90SDK\Samples\C++\DirectShow\BaseClasses\Debug
*/



#include <windows.h>

#include <streams.h>
#include <initguid.h>
#include <olectl.h>
#if (1100 > _MSC_VER)
#include <olectlid.h>
#endif
#include <dvdmedia.h>	// VIDEOINFOHEADER2

#include <xvid.h>		// XviD API

#include "IXvidDecoder.h"
#include "CXvidDecoder.h"
#include "CAbout.h"
#include "config.h"
#include "debug.h"

static bool USE_IYUV;
static bool USE_YV12;
static bool USE_YUY2;
static bool USE_YVYU;
static bool USE_UYVY;
static bool USE_RGB32;
static bool USE_RGB24;
static bool USE_RG555;
static bool USE_RG565;

const AMOVIESETUP_MEDIATYPE sudInputPinTypes[] =
{
    { &MEDIATYPE_Video, &CLSID_XVID },
	{ &MEDIATYPE_Video, &CLSID_XVID_UC },
	{ &MEDIATYPE_Video, &CLSID_DIVX },
	{ &MEDIATYPE_Video, &CLSID_DIVX_UC },
	{ &MEDIATYPE_Video, &CLSID_DX50 },
	{ &MEDIATYPE_Video, &CLSID_DX50_UC },
	{ &MEDIATYPE_Video, &CLSID_MP4V },
  { &MEDIATYPE_Video, &CLSID_MP4V_UC },
};

const AMOVIESETUP_MEDIATYPE sudOutputPinTypes[] =
{
    { &MEDIATYPE_Video, &MEDIASUBTYPE_NULL }
};


const AMOVIESETUP_PIN psudPins[] =
{
	{
		L"Input",           // String pin name
		FALSE,              // Is it rendered
		FALSE,              // Is it an output
		FALSE,              // Allowed none
		FALSE,              // Allowed many
		&CLSID_NULL,        // Connects to filter
		L"Output",          // Connects to pin
		sizeof(sudInputPinTypes) / sizeof(AMOVIESETUP_MEDIATYPE), // Number of types
		&sudInputPinTypes[0]	// The pin details
	},
	{ 
		L"Output",          // String pin name
		FALSE,              // Is it rendered
		TRUE,               // Is it an output
		FALSE,              // Allowed none
		FALSE,              // Allowed many
		&CLSID_NULL,        // Connects to filter
		L"Input",           // Connects to pin
		sizeof(sudOutputPinTypes) / sizeof(AMOVIESETUP_MEDIATYPE),	// Number of types
		sudOutputPinTypes	// The pin details
	}
};


const AMOVIESETUP_FILTER sudXvidDecoder =
{
	&CLSID_XVID,			// Filter CLSID
	XVID_NAME_L,			// Filter name
	MERIT_PREFERRED,		// Its merit
	sizeof(psudPins) / sizeof(AMOVIESETUP_PIN),	// Number of pins
	psudPins				// Pin details
};


// List of class IDs and creator functions for the class factory. This
// provides the link between the OLE entry point in the DLL and an object
// being created. The class factory will call the static CreateInstance

CFactoryTemplate g_Templates[] =
{
	{ 
		XVID_NAME_L,
		&CLSID_XVID,
		CXvidDecoder::CreateInstance,
		NULL,
		&sudXvidDecoder
	},
	{
		XVID_NAME_L L"About",
		&CLSID_CABOUT,
		CAbout::CreateInstance
	}

};


/* note: g_cTemplates must be global; used by strmbase.lib(dllentry.cpp,dllsetup.cpp) */
int g_cTemplates = sizeof(g_Templates) / sizeof(CFactoryTemplate);


STDAPI DllRegisterServer()
{
    return AMovieDllRegisterServer2( TRUE );
}


STDAPI DllUnregisterServer()
{
    return AMovieDllRegisterServer2( FALSE );
}


/* create instance */

CUnknown * WINAPI CXvidDecoder::CreateInstance(LPUNKNOWN punk, HRESULT *phr)
{
    CXvidDecoder * pNewObject = new CXvidDecoder(punk, phr);
    if (pNewObject == NULL)
	{
        *phr = E_OUTOFMEMORY;
    }
    return pNewObject;
}


/* query interfaces */

STDMETHODIMP CXvidDecoder::NonDelegatingQueryInterface(REFIID riid, void **ppv)
{
	CheckPointer(ppv, E_POINTER);

	if (riid == IID_IXvidDecoder)
	{
		return GetInterface((IXvidDecoder *) this, ppv);
	} 
	
	if (riid == IID_ISpecifyPropertyPages)
	{
        return GetInterface((ISpecifyPropertyPages *) this, ppv); 
	} 

	return CVideoTransformFilter::NonDelegatingQueryInterface(riid, ppv);
}



/* constructor */

#define XVID_DLL_NAME "xvidcore.dll"

CXvidDecoder::CXvidDecoder(LPUNKNOWN punk, HRESULT *phr) :
    CVideoTransformFilter(NAME("CXvidDecoder"), punk, CLSID_XVID), m_hdll (NULL)
{
	DPRINTF("Constructor");

    xvid_decore_func = NULL; // Hmm, some strange errors appearing if I try to initialize...
    xvid_global_func = NULL; // ...this in constructor's init-list. So, they assigned here.

    LoadRegistryInfo();

    *phr = OpenLib();
}

HRESULT CXvidDecoder::OpenLib()
{
    DPRINTF("OpenLib");

    if (m_hdll != NULL)
		return E_UNEXPECTED; // Seems, that library already opened.

	xvid_gbl_init_t init;
	memset(&init, 0, sizeof(init));
	init.version = XVID_VERSION;

	m_hdll = LoadLibrary(XVID_DLL_NAME);
	if (m_hdll == NULL) {
		DPRINTF("dll load failed");
		MessageBox(0, XVID_DLL_NAME " not found","Error", MB_TOPMOST);
		return E_FAIL;
	}

	xvid_global_func = (int (__cdecl *)(void *, int, void *, void *))GetProcAddress(m_hdll, "xvid_global");
	if (xvid_global_func == NULL) {
        FreeLibrary(m_hdll);
        m_hdll = NULL;
		MessageBox(0, "xvid_global() not found", "Error", MB_TOPMOST);
		return E_FAIL;
	}

	xvid_decore_func = (int (__cdecl *)(void *, int, void *, void *))GetProcAddress(m_hdll, "xvid_decore");
	if (xvid_decore_func == NULL) {
        xvid_global_func = NULL;
        FreeLibrary(m_hdll);
        m_hdll = NULL;
		MessageBox(0, "xvid_decore() not found", "Error", MB_TOPMOST);
		return E_FAIL;
	}

	if (xvid_global_func(0, XVID_GBL_INIT, &init, NULL) < 0)
	{
        xvid_global_func = NULL;
        xvid_decore_func = NULL;
        FreeLibrary(m_hdll);
        m_hdll = NULL;
		MessageBox(0, "xvid_global() failed", "Error", MB_TOPMOST);
		return E_FAIL;
	}

	memset(&m_create, 0, sizeof(m_create));
	m_create.version = XVID_VERSION;
	m_create.handle = NULL;

	memset(&m_frame, 0, sizeof(m_frame));
	m_frame.version = XVID_VERSION;

	USE_IYUV = false;
	USE_YV12 = false;
	USE_YUY2 = false;
	USE_YVYU = false;
	USE_UYVY = false;
	USE_RGB32 = false;
	USE_RGB24 = false;
	USE_RG555 = false;
	USE_RG565 = false;

	switch ( g_config.nForceColorspace )
	{
	case FORCE_NONE:
		USE_IYUV = true;
		USE_YV12 = true;
		USE_YUY2 = true;
		USE_YVYU = true;
		USE_UYVY = true;
		USE_RGB32 = true;
		USE_RGB24 = true;
		USE_RG555 = true;
		USE_RG565 = true;
		break;
	case FORCE_YV12:
		USE_IYUV = true;
		USE_YV12 = true;
		break;
	case FORCE_YUY2:
		USE_YUY2 = true;
		break;
	case FORCE_RGB24:
		USE_RGB24 = true;
		break;
	case FORCE_RGB32:
		USE_RGB32 = true;
		break;
	}

	switch (g_config.aspect_ratio)
	{
	case 0:
	case 1:
		break;
	case 2:
		ar_x = 4;
		ar_y = 3;
		break;
	case 3:
		ar_x = 16;
		ar_y = 9;
		break;
	case 4:
		ar_x = 47;
		ar_y = 20;
		break;
	}
	
	return S_OK;
}

void CXvidDecoder::CloseLib()
{
	DPRINTF("CloseLib");

	if ((m_create.handle != NULL) && (xvid_decore_func != NULL))
	{
		xvid_decore_func(m_create.handle, XVID_DEC_DESTROY, 0, 0);
		m_create.handle = NULL;
	}

	if (m_hdll != NULL) {
		FreeLibrary(m_hdll);
		m_hdll = NULL;
	}
    xvid_decore_func = NULL;
    xvid_global_func = NULL;
}

/* destructor */

CXvidDecoder::~CXvidDecoder()
{
    DPRINTF("Destructor");
	CloseLib();
}



/* check input type */

HRESULT CXvidDecoder::CheckInputType(const CMediaType * mtIn)
{
	DPRINTF("CheckInputType");
	BITMAPINFOHEADER * hdr;

	ar_x = ar_y = 0;
	
	if (*mtIn->Type() != MEDIATYPE_Video)
	{
		DPRINTF("Error: Unknown Type");
		CloseLib();
		return VFW_E_TYPE_NOT_ACCEPTED;
	}

    if (m_hdll == NULL)
    {
		HRESULT hr = OpenLib();

        if (FAILED(hr) || (m_hdll == NULL)) // Paranoid checks.
			return VFW_E_TYPE_NOT_ACCEPTED;
    }

	if (*mtIn->FormatType() == FORMAT_VideoInfo)
	{
		VIDEOINFOHEADER * vih = (VIDEOINFOHEADER *) mtIn->Format();
		hdr = &vih->bmiHeader;
	}
	else if (*mtIn->FormatType() == FORMAT_VideoInfo2)
	{
		VIDEOINFOHEADER2 * vih2 = (VIDEOINFOHEADER2 *) mtIn->Format();
		hdr = &vih2->bmiHeader;
		if (g_config.aspect_ratio == 0 || g_config.aspect_ratio == 1) {
			ar_x = vih2->dwPictAspectRatioX;
			ar_y = vih2->dwPictAspectRatioY;
		}
		DPRINTF("VIDEOINFOHEADER2 AR: %d:%d", ar_x, ar_y);
	}
  else if (*mtIn->FormatType() == FORMAT_MPEG2Video) {
    MPEG2VIDEOINFO * mpgvi = (MPEG2VIDEOINFO*)mtIn->Format();
    VIDEOINFOHEADER2 * vih2 = &mpgvi->hdr;
		hdr = &vih2->bmiHeader;
		if (g_config.aspect_ratio == 0 || g_config.aspect_ratio == 1) {
			ar_x = vih2->dwPictAspectRatioX;
			ar_y = vih2->dwPictAspectRatioY;
		}
		DPRINTF("VIDEOINFOHEADER2 AR: %d:%d", ar_x, ar_y);

    /* haali media splitter reports VOL information in the format header */

    if (mpgvi->cbSequenceHeader>0) {

      xvid_dec_stats_t stats;
	    memset(&stats, 0, sizeof(stats));
	    stats.version = XVID_VERSION;

	    if (m_create.handle == NULL) {
		    if (xvid_decore_func == NULL)
			    return E_FAIL;
		    if (xvid_decore_func(0, XVID_DEC_CREATE, &m_create, 0) < 0) {
          DPRINTF("*** XVID_DEC_CREATE error");
			    return E_FAIL;
		    }
	    }

      m_frame.general = 0;
      m_frame.bitstream = (void*)mpgvi->dwSequenceHeader;
      m_frame.length = mpgvi->cbSequenceHeader;
      m_frame.output.csp = XVID_CSP_NULL;

      int ret = 0;
      if ((ret=xvid_decore_func(m_create.handle, XVID_DEC_DECODE, &m_frame, &stats)) >= 0) {
        /* honour video dimensions reported in VOL header */
	      if (stats.type == XVID_TYPE_VOL) {
          hdr->biWidth = stats.data.vol.width;
          hdr->biHeight = stats.data.vol.height;
        }
      }
      if (ret == XVID_ERR_MEMORY) return E_FAIL;
    }
  }
	else
	{
		DPRINTF("Error: Unknown FormatType");
		CloseLib();
		return VFW_E_TYPE_NOT_ACCEPTED;
	}

	if (hdr->biHeight < 0)
	{
		DPRINTF("colorspace: inverted input format not supported");
	}

	m_create.width = hdr->biWidth;
	m_create.height = hdr->biHeight;

	switch(hdr->biCompression)
	{
  case FOURCC_mp4v:
	case FOURCC_MP4V:
		if (!(g_config.supported_4cc & SUPPORT_MP4V)) {
			CloseLib();
			return VFW_E_TYPE_NOT_ACCEPTED;
		}
		break;	
	case FOURCC_DIVX :
		if (!(g_config.supported_4cc & SUPPORT_DIVX)) {
			CloseLib();
			return VFW_E_TYPE_NOT_ACCEPTED;
		}
		break;
	case FOURCC_DX50 :
		if (!(g_config.supported_4cc & SUPPORT_DX50)) {
			CloseLib();
			return VFW_E_TYPE_NOT_ACCEPTED;
		}
	case FOURCC_XVID :
		break;
		

	default :
		DPRINTF("Unknown fourcc: 0x%08x (%c%c%c%c)",
			hdr->biCompression,
			(hdr->biCompression)&0xff,
			(hdr->biCompression>>8)&0xff,
			(hdr->biCompression>>16)&0xff,
			(hdr->biCompression>>24)&0xff);
		CloseLib();
		return VFW_E_TYPE_NOT_ACCEPTED;
	}
	return S_OK;
}


/* get list of supported output colorspaces */


HRESULT CXvidDecoder::GetMediaType(int iPosition, CMediaType *mtOut)
{
	BITMAPINFOHEADER * bmih;
	DPRINTF("GetMediaType");

	if (m_pInput->IsConnected() == FALSE)
	{
		return E_UNEXPECTED;
	}

	if (!g_config.videoinfo_compat) {
		VIDEOINFOHEADER2 * vih = (VIDEOINFOHEADER2 *) mtOut->ReallocFormatBuffer(sizeof(VIDEOINFOHEADER2));
		if (vih == NULL) return E_OUTOFMEMORY;

		ZeroMemory(vih, sizeof (VIDEOINFOHEADER2));
		bmih = &(vih->bmiHeader);
		mtOut->SetFormatType(&FORMAT_VideoInfo2);

		if (ar_x != 0 && ar_y != 0) {
			vih->dwPictAspectRatioX = ar_x;
			vih->dwPictAspectRatioY = ar_y;
			forced_ar = true;
		} else { // just to be safe
			vih->dwPictAspectRatioX = m_create.width;
			vih->dwPictAspectRatioY = abs(m_create.height);
			forced_ar = false;
		} 
	} else {

		VIDEOINFOHEADER * vih = (VIDEOINFOHEADER *) mtOut->ReallocFormatBuffer(sizeof(VIDEOINFOHEADER));
		if (vih == NULL) return E_OUTOFMEMORY;

		ZeroMemory(vih, sizeof (VIDEOINFOHEADER));
		bmih = &(vih->bmiHeader);
		mtOut->SetFormatType(&FORMAT_VideoInfo);
	}

	bmih->biSize = sizeof(BITMAPINFOHEADER);
	bmih->biWidth	= m_create.width;
	bmih->biHeight = m_create.height;
	bmih->biPlanes = 1;

	if (iPosition < 0) return E_INVALIDARG;

	switch(iPosition)
	{

	case 0:
if ( USE_YUY2 )
{
		bmih->biCompression = MEDIASUBTYPE_YUY2.Data1;
		bmih->biBitCount = 16;
		mtOut->SetSubtype(&MEDIASUBTYPE_YUY2);
		break;
}
	case 1 :
if ( USE_YVYU )
{
		bmih->biCompression = MEDIASUBTYPE_YVYU.Data1;
		bmih->biBitCount = 16;
		mtOut->SetSubtype(&MEDIASUBTYPE_YVYU);
		break;
}
	case 2 :
if ( USE_UYVY )
{
		bmih->biCompression = MEDIASUBTYPE_UYVY.Data1;
		bmih->biBitCount = 16;
		mtOut->SetSubtype(&MEDIASUBTYPE_UYVY);
		break;
}
	case 3	:
		if ( USE_IYUV )
{
		bmih->biCompression = CLSID_MEDIASUBTYPE_IYUV.Data1;
		bmih->biBitCount = 12;
		mtOut->SetSubtype(&CLSID_MEDIASUBTYPE_IYUV);
		break;
}
	case 4	:
if ( USE_YV12 )
{
		bmih->biCompression = MEDIASUBTYPE_YV12.Data1;
		bmih->biBitCount = 12;
		mtOut->SetSubtype(&MEDIASUBTYPE_YV12);
		break;
}
	case 5 :
if ( USE_RGB32 )
{
		bmih->biCompression = BI_RGB;
		bmih->biBitCount = 32;
		mtOut->SetSubtype(&MEDIASUBTYPE_RGB32);
		break;
}
	case 6 :
if ( USE_RGB24 )
{
		bmih->biCompression = BI_RGB;
		bmih->biBitCount = 24;	
		mtOut->SetSubtype(&MEDIASUBTYPE_RGB24);
		break;
}
	case 7 :
if ( USE_RG555 )
{
		bmih->biCompression = BI_RGB;
		bmih->biBitCount = 16;	
		mtOut->SetSubtype(&MEDIASUBTYPE_RGB555);
		break;
}
	case 8 :
if ( USE_RG565 )
{
		bmih->biCompression = BI_RGB;
		bmih->biBitCount = 16;	
		mtOut->SetSubtype(&MEDIASUBTYPE_RGB565);
		break;
}	
	default :
		return VFW_S_NO_MORE_ITEMS;
	}

	bmih->biSizeImage = GetBitmapSize(bmih);

	mtOut->SetType(&MEDIATYPE_Video);
	mtOut->SetTemporalCompression(FALSE);
	mtOut->SetSampleSize(bmih->biSizeImage);

	return S_OK;
}


/* (internal function) change colorspace */
#define CALC_BI_STRIDE(width,bitcount)  ((((width * bitcount) + 31) & ~31) >> 3)

HRESULT CXvidDecoder::ChangeColorspace(GUID subtype, GUID formattype, void * format)
{
	DWORD biWidth;

	if (formattype == FORMAT_VideoInfo)
	{
		VIDEOINFOHEADER * vih = (VIDEOINFOHEADER * )format;
		biWidth = vih->bmiHeader.biWidth;
		m_frame.output.stride[0] = CALC_BI_STRIDE(vih->bmiHeader.biWidth, vih->bmiHeader.biBitCount);
		rgb_flip = (vih->bmiHeader.biHeight < 0 ? 0 : XVID_CSP_VFLIP);
	}
	else if (formattype == FORMAT_VideoInfo2)
	{
		VIDEOINFOHEADER2 * vih2 = (VIDEOINFOHEADER2 * )format;
		biWidth = vih2->bmiHeader.biWidth;
		m_frame.output.stride[0] = CALC_BI_STRIDE(vih2->bmiHeader.biWidth, vih2->bmiHeader.biBitCount);
		rgb_flip = (vih2->bmiHeader.biHeight < 0 ? 0 : XVID_CSP_VFLIP);
	}
	else
	{
		return S_FALSE;
	}

	if (subtype == CLSID_MEDIASUBTYPE_IYUV)
	{
		DPRINTF("IYUV");
		rgb_flip = 0;
		m_frame.output.csp = XVID_CSP_I420;
		m_frame.output.stride[0] = CALC_BI_STRIDE(biWidth, 8);	/* planar format fix */
	}
	else if (subtype == MEDIASUBTYPE_YV12)
	{
		DPRINTF("YV12");
		rgb_flip = 0;
		m_frame.output.csp = XVID_CSP_YV12;
		m_frame.output.stride[0] = CALC_BI_STRIDE(biWidth, 8);	/* planar format fix */
	}
	else if (subtype == MEDIASUBTYPE_YUY2)
	{
		DPRINTF("YUY2");
		rgb_flip = 0;
		m_frame.output.csp = XVID_CSP_YUY2;
	}
	else if (subtype == MEDIASUBTYPE_YVYU)
	{
		DPRINTF("YVYU");
		rgb_flip = 0;
		m_frame.output.csp = XVID_CSP_YVYU;
	}
	else if (subtype == MEDIASUBTYPE_UYVY)
	{
		DPRINTF("UYVY");
		rgb_flip = 0;
		m_frame.output.csp = XVID_CSP_UYVY;
	}
	else if (subtype == MEDIASUBTYPE_RGB32)
	{
		DPRINTF("RGB32");
		m_frame.output.csp = rgb_flip | XVID_CSP_BGRA;
	}
	else if (subtype == MEDIASUBTYPE_RGB24)
	{
		DPRINTF("RGB24");
		m_frame.output.csp = rgb_flip | XVID_CSP_BGR;
	}
	else if (subtype == MEDIASUBTYPE_RGB555)
	{
		DPRINTF("RGB555");
		m_frame.output.csp = rgb_flip | XVID_CSP_RGB555;
	}
	else if (subtype == MEDIASUBTYPE_RGB565)
	{
		DPRINTF("RGB565");
		m_frame.output.csp = rgb_flip | XVID_CSP_RGB565;
	}
	else if (subtype == GUID_NULL)
	{
		m_frame.output.csp = XVID_CSP_NULL;
	}
	else
	{
		return S_FALSE;
	}

	return S_OK;
}


/* set output colorspace */

HRESULT CXvidDecoder::SetMediaType(PIN_DIRECTION direction, const CMediaType *pmt)
{
	DPRINTF("SetMediaType");
	
	if (direction == PINDIR_OUTPUT)
	{
		return ChangeColorspace(*pmt->Subtype(), *pmt->FormatType(), pmt->Format());
	}
	
	return S_OK;
}


/* check input<->output compatiblity */

HRESULT CXvidDecoder::CheckTransform(const CMediaType *mtIn, const CMediaType *mtOut)
{
	DPRINTF("CheckTransform");
	return S_OK;
}


/* alloc output buffer */

HRESULT CXvidDecoder::DecideBufferSize(IMemAllocator *pAlloc, ALLOCATOR_PROPERTIES *ppropInputRequest)
{
	DPRINTF("DecideBufferSize");
	HRESULT result;
	ALLOCATOR_PROPERTIES ppropActual;

	if (m_pInput->IsConnected() == FALSE)
	{
		return E_UNEXPECTED;
	}

	ppropInputRequest->cBuffers = 1;
	ppropInputRequest->cbBuffer = m_create.width * m_create.height * 4;
	// cbAlign causes problems with the resize filter */
	// ppropInputRequest->cbAlign = 16;	
	ppropInputRequest->cbPrefix = 0;
		
	result = pAlloc->SetProperties(ppropInputRequest, &ppropActual);
	if (result != S_OK)
	{
		return result;
	}

	if (ppropActual.cbBuffer < ppropInputRequest->cbBuffer)
	{
		return E_FAIL;
	}

	return S_OK;
}


/* decode frame */

HRESULT CXvidDecoder::Transform(IMediaSample *pIn, IMediaSample *pOut)
{
	DPRINTF("Transform");
	xvid_dec_stats_t stats;
    int length;

	memset(&stats, 0, sizeof(stats));
	stats.version = XVID_VERSION;

	if (m_create.handle == NULL)
	{
		if (xvid_decore_func == NULL)
			return E_FAIL;

		if (xvid_decore_func(0, XVID_DEC_CREATE, &m_create, 0) < 0)
		{
            DPRINTF("*** XVID_DEC_CREATE error");
			return E_FAIL;
		}
	}

	AM_MEDIA_TYPE * mtOut;
	pOut->GetMediaType(&mtOut);
	if (mtOut != NULL)
	{
		HRESULT result;

		result = ChangeColorspace(mtOut->subtype, mtOut->formattype, mtOut->pbFormat);
		DeleteMediaType(mtOut);

		if (result != S_OK)
		{
            DPRINTF("*** ChangeColorspace error");
			return result;
		}
	}
	
	m_frame.length = pIn->GetActualDataLength();
	if (pIn->GetPointer((BYTE**)&m_frame.bitstream) != S_OK)
	{
		return S_FALSE;
	}

	if (pOut->GetPointer((BYTE**)&m_frame.output.plane[0]) != S_OK)
	{
		return S_FALSE; 
	}

	m_frame.general = XVID_LOWDELAY;

	if (pIn->IsDiscontinuity() == S_OK)
		m_frame.general |= XVID_DISCONTINUITY;

	if (g_config.nDeblock_Y)
		m_frame.general |= XVID_DEBLOCKY;

	if (g_config.nDeblock_UV)
		m_frame.general |= XVID_DEBLOCKUV;

	if (g_config.nDering_Y)
		m_frame.general |= XVID_DERINGY;

	if (g_config.nDering_UV)
		m_frame.general |= XVID_DERINGUV;

	if (g_config.nFilmEffect)
		m_frame.general |= XVID_FILMEFFECT;

	m_frame.brightness = g_config.nBrightness;

	m_frame.output.csp &= ~XVID_CSP_VFLIP;
	m_frame.output.csp |= rgb_flip^(g_config.nFlipVideo ? XVID_CSP_VFLIP : 0);

    // Paranoid check.
    if (xvid_decore_func == NULL)
		return E_FAIL;



repeat :

	if (pIn->IsPreroll() != S_OK)
	{
		length = xvid_decore_func(m_create.handle, XVID_DEC_DECODE, &m_frame, &stats);
                
		if (length == XVID_ERR_MEMORY)
			return E_FAIL;
		else if (length < 0)
		{
            DPRINTF("*** XVID_DEC_DECODE");
			return S_FALSE;
		} else 
			if (g_config.aspect_ratio == 0 || g_config.aspect_ratio == 1 && forced_ar == false) {
        
      if (stats.type != XVID_TYPE_NOTHING) {  /* dont attempt to set vmr aspect ratio if no frame was returned by decoder */
			// inspired by minolta! works for VMR 7 + 9
			IMediaSample2 *pOut2 = NULL;
			AM_SAMPLE2_PROPERTIES outProp2;
			if (SUCCEEDED(pOut->QueryInterface(IID_IMediaSample2, (void **)&pOut2)) &&
				SUCCEEDED(pOut2->GetProperties(FIELD_OFFSET(AM_SAMPLE2_PROPERTIES, tStart), (PBYTE)&outProp2)))
			{
				CMediaType mtOut2 = m_pOutput->CurrentMediaType();
				VIDEOINFOHEADER2* vihOut2 = (VIDEOINFOHEADER2*)mtOut2.Format();

				if (*mtOut2.FormatType() == FORMAT_VideoInfo2 && 
					vihOut2->dwPictAspectRatioX != ar_x && vihOut2->dwPictAspectRatioY != ar_y)
				{
					vihOut2->dwPictAspectRatioX = ar_x;
					vihOut2->dwPictAspectRatioY = ar_y;
					pOut2->SetMediaType(&mtOut2);
					m_pOutput->SetMediaType(&mtOut2);
				}
				pOut2->Release();
			}
      }
		}
	}
	else
	{	/* Preroll frame - won't be displayed */
		int tmp = m_frame.output.csp;
		int tmp_gen = m_frame.general;

		m_frame.output.csp = XVID_CSP_NULL;

		/* Disable postprocessing to speed-up seeking */
		m_frame.general &= ~XVID_DEBLOCKY;
		m_frame.general &= ~XVID_DEBLOCKUV;
		/*m_frame.general &= ~XVID_DERING;*/
		m_frame.general &= ~XVID_FILMEFFECT;

		length = xvid_decore_func(m_create.handle, XVID_DEC_DECODE, &m_frame, &stats);
		if (length == XVID_ERR_MEMORY)
			return E_FAIL;
		else if (length < 0)
		{
            DPRINTF("*** XVID_DEC_DECODE");
			return S_FALSE;
		}

		m_frame.output.csp = tmp;
		m_frame.general = tmp_gen;
	}

	if (stats.type == XVID_TYPE_NOTHING && length > 0) {
		DPRINTF(" B-Frame decoder lag");
		return S_FALSE;
	}


	if (stats.type == XVID_TYPE_VOL)
	{
		if (stats.data.vol.width != m_create.width ||
			stats.data.vol.height != m_create.height)
		{
			DPRINTF("TODO: auto-resize");
			return S_FALSE;
		}

		pOut->SetSyncPoint(TRUE);

		if (g_config.aspect_ratio == 0 || g_config.aspect_ratio == 1) { /* auto */
			int par_x, par_y;
			if (stats.data.vol.par == XVID_PAR_EXT) {
				par_x = stats.data.vol.par_width;
				par_y = stats.data.vol.par_height;
			} else {
				par_x = PARS[stats.data.vol.par-1][0];
				par_y = PARS[stats.data.vol.par-1][1];
			}

			ar_x = par_x * stats.data.vol.width;
			ar_y = par_y * stats.data.vol.height;
		}

		m_frame.bitstream = (BYTE*)m_frame.bitstream + length;
		m_frame.length -= length;
		goto repeat;
	}

	if (pIn->IsPreroll() == S_OK) {
		return S_FALSE;
	}

	return S_OK;
}


/* get property page list */

STDMETHODIMP CXvidDecoder::GetPages(CAUUID * pPages)
{
	DPRINTF("GetPages");

	pPages->cElems = 1;
	pPages->pElems = (GUID *)CoTaskMemAlloc(pPages->cElems * sizeof(GUID));
	if (pPages->pElems == NULL)
	{
		return E_OUTOFMEMORY;
	}
	pPages->pElems[0] = CLSID_CABOUT;

	return S_OK;
}


/* cleanup pages */

STDMETHODIMP CXvidDecoder::FreePages(CAUUID * pPages)
{
	DPRINTF("FreePages");
	CoTaskMemFree(pPages->pElems);
	return S_OK;
}
