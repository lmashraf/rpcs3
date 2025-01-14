﻿#include "stdafx.h"
#include "BufferUtils.h"
#include "../rsx_methods.h"
#include "Utilities/sysinfo.h"
#include "../RSXThread.h"

#include <limits>

#define DEBUG_VERTEX_STREAMING 0

const bool s_use_ssse3 =
#ifdef _MSC_VER
	utils::has_ssse3();
#elif __SSSE3__
	true;
#else
	false;
#define _mm_shuffle_epi8
#endif

namespace
{
	// FIXME: GSL as_span break build if template parameter is non const with current revision.
	// Replace with true as_span when fixed.
	template <typename T>
	gsl::span<T> as_span_workaround(gsl::span<gsl::byte> unformated_span)
	{
		return{ (T*)unformated_span.data(), ::narrow<int>(unformated_span.size_bytes() / sizeof(T)) };
	}

	template <typename T>
	gsl::span<T> as_const_span(gsl::span<const gsl::byte> unformated_span)
	{
		return{ (T*)unformated_span.data(), ::narrow<int>(unformated_span.size_bytes() / sizeof(T)) };
	}
}

namespace
{
	/**
	 * Convert CMP vector to RGBA16.
	 * A vector in CMP (compressed) format is stored as X11Y11Z10 and has a W component of 1.
	 * X11 and Y11 channels are int between -1024 and 1023 interpreted as -1.f, 1.f
	 * Z10 is int between -512 and 511 interpreted as -1.f, 1.f
	 */
	std::array<u16, 4> decode_cmp_vector(u32 encoded_vector)
	{
		u16 Z = encoded_vector >> 22;
		Z = Z << 6;
		u16 Y = (encoded_vector >> 11) & 0x7FF;
		Y = Y << 5;
		u16 X = encoded_vector & 0x7FF;
		X = X << 5;
		return{ X, Y, Z, 1 };
	}

	inline void stream_data_to_memory_swapped_u32(void *dst, const void *src, u32 vertex_count, u8 stride)
	{
		const __m128i mask = _mm_set_epi8(
			0xC, 0xD, 0xE, 0xF,
			0x8, 0x9, 0xA, 0xB,
			0x4, 0x5, 0x6, 0x7,
			0x0, 0x1, 0x2, 0x3);

		__m128i* dst_ptr = (__m128i*)dst;
		__m128i* src_ptr = (__m128i*)src;

		const u32 dword_count = (vertex_count * (stride >> 2));
		const u32 iterations = dword_count >> 2;
		const u32 remaining = dword_count % 4;

		if (LIKELY(s_use_ssse3))
		{
			for (u32 i = 0; i < iterations; ++i)
			{
				const __m128i vector = _mm_loadu_si128(src_ptr);
				const __m128i shuffled_vector = _mm_shuffle_epi8(vector, mask);
				_mm_stream_si128(dst_ptr, shuffled_vector);

				src_ptr++;
				dst_ptr++;
			}
		}
		else
		{
			for (u32 i = 0; i < iterations; ++i)
			{
				const __m128i vec0 = _mm_loadu_si128(src_ptr);
				const __m128i vec1 = _mm_or_si128(_mm_slli_epi16(vec0, 8), _mm_srli_epi16(vec0, 8));
				const __m128i vec2 = _mm_or_si128(_mm_slli_epi32(vec1, 16), _mm_srli_epi32(vec1, 16));
				_mm_stream_si128(dst_ptr, vec2);

				src_ptr++;
				dst_ptr++;
			}
		}

		if (remaining)
		{
			u32 *src_ptr2 = (u32 *)src_ptr;
			u32 *dst_ptr2 = (u32 *)dst_ptr;

			for (u32 i = 0; i < remaining; ++i)
				dst_ptr2[i] = se_storage<u32>::swap(src_ptr2[i]);
		}
	}

	inline void stream_data_to_memory_swapped_u16(void *dst, const void *src, u32 vertex_count, u8 stride)
	{
		const __m128i mask = _mm_set_epi8(
			0xE, 0xF, 0xC, 0xD,
			0xA, 0xB, 0x8, 0x9,
			0x6, 0x7, 0x4, 0x5,
			0x2, 0x3, 0x0, 0x1);

		__m128i* dst_ptr = (__m128i*)dst;
		__m128i* src_ptr = (__m128i*)src;

		const u32 word_count = (vertex_count * (stride >> 1));
		const u32 iterations = word_count >> 3;
		const u32 remaining = word_count % 8;

		if (LIKELY(s_use_ssse3))
		{
			for (u32 i = 0; i < iterations; ++i)
			{
				const __m128i vector = _mm_loadu_si128(src_ptr);
				const __m128i shuffled_vector = _mm_shuffle_epi8(vector, mask);
				_mm_stream_si128(dst_ptr, shuffled_vector);

				src_ptr++;
				dst_ptr++;
			}
		}
		else
		{
			for (u32 i = 0; i < iterations; ++i)
			{
				const __m128i vec0 = _mm_loadu_si128(src_ptr);
				const __m128i vec1 = _mm_or_si128(_mm_slli_epi16(vec0, 8), _mm_srli_epi16(vec0, 8));
				_mm_stream_si128(dst_ptr, vec1);

				src_ptr++;
				dst_ptr++;
			}
		}

		if (remaining)
		{
			u16 *src_ptr2 = (u16 *)src_ptr;
			u16 *dst_ptr2 = (u16 *)dst_ptr;

			for (u32 i = 0; i < remaining; ++i)
				dst_ptr2[i] = se_storage<u16>::swap(src_ptr2[i]);
		}
	}

	inline void stream_data_to_memory_swapped_u32_non_continuous(void *dst, const void *src, u32 vertex_count, u8 dst_stride, u8 src_stride)
	{
		const __m128i mask = _mm_set_epi8(
			0xC, 0xD, 0xE, 0xF,
			0x8, 0x9, 0xA, 0xB,
			0x4, 0x5, 0x6, 0x7,
			0x0, 0x1, 0x2, 0x3);

		char *src_ptr = (char *)src;
		char *dst_ptr = (char *)dst;

		//Count vertices to copy
		const bool is_128_aligned = !((dst_stride | src_stride) & 15);

		u32 min_block_size = std::min(src_stride, dst_stride);
		if (min_block_size == 0) min_block_size = dst_stride;

		u32 iterations = 0;
		u32 remainder = is_128_aligned ? 0 : 1 + ((16 - min_block_size) / min_block_size);

		if (vertex_count > remainder)
			iterations = vertex_count - remainder;
		else
			remainder = vertex_count;

		if (LIKELY(s_use_ssse3))
		{
			for (u32 i = 0; i < iterations; ++i)
			{
				const __m128i vector = _mm_loadu_si128((__m128i*)src_ptr);
				const __m128i shuffled_vector = _mm_shuffle_epi8(vector, mask);
				_mm_storeu_si128((__m128i*)dst_ptr, shuffled_vector);

				src_ptr += src_stride;
				dst_ptr += dst_stride;
			}
		}
		else
		{
			for (u32 i = 0; i < iterations; ++i)
			{
				const __m128i vec0 = _mm_loadu_si128((__m128i*)src_ptr);
				const __m128i vec1 = _mm_or_si128(_mm_slli_epi16(vec0, 8), _mm_srli_epi16(vec0, 8));
				const __m128i vec2 = _mm_or_si128(_mm_slli_epi32(vec1, 16), _mm_srli_epi32(vec1, 16));
				_mm_storeu_si128((__m128i*)dst_ptr, vec2);

				src_ptr += src_stride;
				dst_ptr += dst_stride;
			}
		}

		if (remainder)
		{
			const u8 attribute_sz = min_block_size >> 2;
			for (u32 n = 0; n < remainder; ++n)
			{
				for (u32 v= 0; v < attribute_sz; ++v)
					((u32*)dst_ptr)[v] = ((be_t<u32>*)src_ptr)[v];

				src_ptr += src_stride;
				dst_ptr += dst_stride;
			}
		}
	}

	inline void stream_data_to_memory_swapped_u16_non_continuous(void *dst, const void *src, u32 vertex_count, u8 dst_stride, u8 src_stride)
	{
		const __m128i mask = _mm_set_epi8(
			0xE, 0xF, 0xC, 0xD,
			0xA, 0xB, 0x8, 0x9,
			0x6, 0x7, 0x4, 0x5,
			0x2, 0x3, 0x0, 0x1);

		char *src_ptr = (char *)src;
		char *dst_ptr = (char *)dst;

		const bool is_128_aligned = !((dst_stride | src_stride) & 15);

		u32 min_block_size = std::min(src_stride, dst_stride);
		if (min_block_size == 0) min_block_size = dst_stride;

		u32 iterations = 0;
		u32 remainder = is_128_aligned ? 0 : 1 + ((16 - min_block_size) / min_block_size);

		if (vertex_count > remainder)
			iterations = vertex_count - remainder;
		else
			remainder = vertex_count;

		if (LIKELY(s_use_ssse3))
		{
			for (u32 i = 0; i < iterations; ++i)
			{
				const __m128i vector = _mm_loadu_si128((__m128i*)src_ptr);
				const __m128i shuffled_vector = _mm_shuffle_epi8(vector, mask);
				_mm_storeu_si128((__m128i*)dst_ptr, shuffled_vector);

				src_ptr += src_stride;
				dst_ptr += dst_stride;
			}
		}
		else
		{
			for (u32 i = 0; i < iterations; ++i)
			{
				const __m128i vec0 = _mm_loadu_si128((__m128i*)src_ptr);
				const __m128i vec1 = _mm_or_si128(_mm_slli_epi16(vec0, 8), _mm_srli_epi16(vec0, 8));
				_mm_storeu_si128((__m128i*)dst_ptr, vec1);

				src_ptr += src_stride;
				dst_ptr += dst_stride;
			}
		}

		if (remainder)
		{
			const u8 attribute_sz = min_block_size >> 1;
			for (u32 n = 0; n < remainder; ++n)
			{
				for (u32 v = 0; v < attribute_sz; ++v)
					((u16*)dst_ptr)[v] = ((be_t<u16>*)src_ptr)[v];

				src_ptr += src_stride;
				dst_ptr += dst_stride;
			}
		}
	}

	inline void stream_data_to_memory_u8_non_continuous(void *dst, const void *src, u32 vertex_count, u8 attribute_size, u8 dst_stride, u8 src_stride)
	{
		char *src_ptr = (char *)src;
		char *dst_ptr = (char *)dst;

		switch (attribute_size)
		{
			case 4:
			{
				//Read one dword every iteration
				for (u32 vertex = 0; vertex < vertex_count; ++vertex)
				{
					*(u32*)dst_ptr = *(u32*)src_ptr;

					dst_ptr += dst_stride;
					src_ptr += src_stride;
				}

				break;
			}
			case 3:
			{
				//Read one word and one byte
				for (u32 vertex = 0; vertex < vertex_count; ++vertex)
				{
					*(u16*)dst_ptr = *(u16*)src_ptr;
					dst_ptr[2] = src_ptr[2];

					dst_ptr += dst_stride;
					src_ptr += src_stride;
				}

				break;
			}
			case 2:
			{
				//Copy u16 blocks
				for (u32 vertex = 0; vertex < vertex_count; ++vertex)
				{
					*(u32*)dst_ptr = *(u32*)src_ptr;

					dst_ptr += dst_stride;
					src_ptr += src_stride;
				}

				break;
			}
			case 1:
			{
				for (u32 vertex = 0; vertex < vertex_count; ++vertex)
				{
					dst_ptr[0] = src_ptr[0];

					dst_ptr += dst_stride;
					src_ptr += src_stride;
				}

				break;
			}
		}
	}

	template <typename T, typename U, int N>
	void copy_whole_attribute_array_impl(void *raw_dst, void *raw_src, u8 dst_stride, u32 src_stride, u32 vertex_count)
	{
		char *src_ptr = (char *)raw_src;
		char *dst_ptr = (char *)raw_dst;

		for (u32 vertex = 0; vertex < vertex_count; ++vertex)
		{
			T* typed_dst = (T*)dst_ptr;
			U* typed_src = (U*)src_ptr;

			for (u32 i = 0; i < N; ++i)
			{
				typed_dst[i] = typed_src[i];
			}

			src_ptr += src_stride;
			dst_ptr += dst_stride;
		}
	}

	/*
	 * Copies a number of src vertices, repeated over and over to fill the dst
	 * e.g repeat 2 vertices over a range of 16 verts, so 8 reps
	 */
	template <typename T, typename U, int N>
	void copy_whole_attribute_array_repeating_impl(void *raw_dst, void *raw_src, const u8 dst_stride, const u32 src_stride, const u32 vertex_count, const u32 src_vertex_count)
	{
		char *src_ptr = (char *)raw_src;
		char *dst_ptr = (char *)raw_dst;

		u32 src_offset = 0;
		u32 src_limit = src_stride * src_vertex_count;

		for (u32 vertex = 0; vertex < vertex_count; ++vertex)
		{
			T* typed_dst = (T*)dst_ptr;
			U* typed_src = (U*)(src_ptr + src_offset);

			for (u32 i = 0; i < N; ++i)
			{
				typed_dst[i] = typed_src[i];
			}

			src_offset = (src_offset + src_stride) % src_limit;
			dst_ptr += dst_stride;
		}
	}

	template <typename U, typename T>
	void copy_whole_attribute_array(void *raw_dst, void *raw_src, const u8 attribute_size, const u8 dst_stride, const u32 src_stride, const u32 vertex_count, const u32 src_vertex_count)
	{
		//Eliminate the inner loop by templating the inner loop counter N

		if (src_vertex_count == vertex_count)
		{
			switch (attribute_size)
			{
			case 1:
				copy_whole_attribute_array_impl<U, T, 1>(raw_dst, raw_src, dst_stride, src_stride, vertex_count);
				break;
			case 2:
				copy_whole_attribute_array_impl<U, T, 2>(raw_dst, raw_src, dst_stride, src_stride, vertex_count);
				break;
			case 3:
				copy_whole_attribute_array_impl<U, T, 3>(raw_dst, raw_src, dst_stride, src_stride, vertex_count);
				break;
			case 4:
				copy_whole_attribute_array_impl<U, T, 4>(raw_dst, raw_src, dst_stride, src_stride, vertex_count);
				break;
			}
		}
		else
		{
			switch (attribute_size)
			{
			case 1:
				copy_whole_attribute_array_repeating_impl<U, T, 1>(raw_dst, raw_src, dst_stride, src_stride, vertex_count, src_vertex_count);
				break;
			case 2:
				copy_whole_attribute_array_repeating_impl<U, T, 2>(raw_dst, raw_src, dst_stride, src_stride, vertex_count, src_vertex_count);
				break;
			case 3:
				copy_whole_attribute_array_repeating_impl<U, T, 3>(raw_dst, raw_src, dst_stride, src_stride, vertex_count, src_vertex_count);
				break;
			case 4:
				copy_whole_attribute_array_repeating_impl<U, T, 4>(raw_dst, raw_src, dst_stride, src_stride, vertex_count, src_vertex_count);
				break;
			}
		}
	}
}

void write_vertex_array_data_to_buffer(gsl::span<gsl::byte> raw_dst_span, gsl::span<const gsl::byte> src_ptr, u32 count, rsx::vertex_base_type type, u32 vector_element_count, u32 attribute_src_stride, u8 dst_stride, bool swap_endianness)
{
	verify(HERE), (vector_element_count > 0);
	const u32 src_read_stride = rsx::get_vertex_type_size_on_host(type, vector_element_count);

	bool use_stream_no_stride = false;
	bool use_stream_with_stride = false;

	//If stride is not defined, we have a packed array
	if (attribute_src_stride == 0) attribute_src_stride = src_read_stride;

	//Sometimes, we get a vertex attribute to be repeated. Just copy the supplied vertices only
	//TODO: Stop these requests from getting here in the first place!
	//TODO: Check if it is possible to have a repeating array with more than one attribute instance
	const u32 real_count = (u32)src_ptr.size_bytes() / attribute_src_stride;
	if (real_count == 1) attribute_src_stride = 0;	//Always fetch src[0]

	//TODO: Determine favourable vertex threshold where vector setup costs become negligible
	//Tests show that even with 4 vertices, using traditional bswap is significantly slower over a large number of calls

	const u64 src_address = (u64)src_ptr.data();
	const bool sse_aligned = ((src_address & 15) == 0);

#if !DEBUG_VERTEX_STREAMING

	if (swap_endianness)
	{
		if (real_count >= count || real_count == 1)
		{
			if (attribute_src_stride == dst_stride && src_read_stride == dst_stride)
				use_stream_no_stride = true;
			else
				use_stream_with_stride = true;
		}
	}

#endif

	switch (type)
	{
	case rsx::vertex_base_type::ub:
	case rsx::vertex_base_type::ub256:
	{
		if (use_stream_no_stride)
			memcpy(raw_dst_span.data(), src_ptr.data(), count * dst_stride);
		else if (use_stream_with_stride)
			stream_data_to_memory_u8_non_continuous(raw_dst_span.data(), src_ptr.data(), count, vector_element_count, dst_stride, attribute_src_stride);
		else
			copy_whole_attribute_array<u8, u8>((void *)raw_dst_span.data(), (void *)src_ptr.data(), vector_element_count, dst_stride, attribute_src_stride, count, real_count);

		return;
	}
	case rsx::vertex_base_type::s1:
	case rsx::vertex_base_type::sf:
	case rsx::vertex_base_type::s32k:
	{
		if (use_stream_no_stride && sse_aligned)
			stream_data_to_memory_swapped_u16(raw_dst_span.data(), src_ptr.data(), count, attribute_src_stride);
		else if (use_stream_with_stride)
			stream_data_to_memory_swapped_u16_non_continuous(raw_dst_span.data(), src_ptr.data(), count, dst_stride, attribute_src_stride);
		else if (swap_endianness)
			copy_whole_attribute_array<be_t<u16>, u16>((void *)raw_dst_span.data(), (void *)src_ptr.data(), vector_element_count, dst_stride, attribute_src_stride, count, real_count);
		else
			copy_whole_attribute_array<u16, u16>((void *)raw_dst_span.data(), (void *)src_ptr.data(), vector_element_count, dst_stride, attribute_src_stride, count, real_count);

		return;
	}
	case rsx::vertex_base_type::f:
	{
		if (use_stream_no_stride && sse_aligned)
			stream_data_to_memory_swapped_u32(raw_dst_span.data(), src_ptr.data(), count, attribute_src_stride);
		else if (use_stream_with_stride)
			stream_data_to_memory_swapped_u32_non_continuous(raw_dst_span.data(), src_ptr.data(), count, dst_stride, attribute_src_stride);
		else if (swap_endianness)
			copy_whole_attribute_array<be_t<u32>, u32>((void *)raw_dst_span.data(), (void *)src_ptr.data(), vector_element_count, dst_stride, attribute_src_stride, count, real_count);
		else
			copy_whole_attribute_array<u32, u32>((void *)raw_dst_span.data(), (void *)src_ptr.data(), vector_element_count, dst_stride, attribute_src_stride, count, real_count);

		return;
	}
	case rsx::vertex_base_type::cmp:
	{
		gsl::span<u16> dst_span = as_span_workaround<u16>(raw_dst_span);
		for (u32 i = 0; i < count; ++i)
		{
			u32 src_value;
			memcpy(&src_value, src_ptr.subspan(attribute_src_stride * i).data(), sizeof(u32));

			if (swap_endianness) src_value = se_storage<u32>::swap(src_value);

			const auto& decoded_vector                 = decode_cmp_vector(src_value);
			dst_span[i * dst_stride / sizeof(u16)] = decoded_vector[0];
			dst_span[i * dst_stride / sizeof(u16) + 1] = decoded_vector[1];
			dst_span[i * dst_stride / sizeof(u16) + 2] = decoded_vector[2];
			dst_span[i * dst_stride / sizeof(u16) + 3] = decoded_vector[3];
		}
		return;
	}
	}
}

namespace
{
	template <typename T>
	constexpr T index_limit()
	{
		return std::numeric_limits<T>::max();
	}

	template <typename T>
	const T& min_max(T& min, T& max, const T& value)
	{
		if (value < min)
			min = value;

		if (value > max)
			max = value;

		return value;
	}

	struct untouched_impl
	{
		static
		std::tuple<u16, u16, u32> upload_u16_swapped(const void *src, void *dst, u32 count)
		{
			const __m128i mask = _mm_set_epi8(
				0xE, 0xF, 0xC, 0xD,
				0xA, 0xB, 0x8, 0x9,
				0x6, 0x7, 0x4, 0x5,
				0x2, 0x3, 0x0, 0x1);

			auto src_stream = (const __m128i*)src;
			auto dst_stream = (__m128i*)dst;

			__m128i min = _mm_set1_epi16(0xFFFF);
			__m128i max = _mm_set1_epi16(0);

			const auto iterations = count / 8;
			for (unsigned n = 0; n < iterations; ++n)
			{
				const __m128i raw = _mm_loadu_si128(src_stream++);
				const __m128i value = _mm_shuffle_epi8(raw, mask);
				max = _mm_max_epu16(max, value);
				min = _mm_min_epu16(min, value);
				_mm_storeu_si128(dst_stream++, value);
			}

			const __m128i mask_step1 = _mm_set_epi8(
				0, 0, 0, 0, 0, 0, 0, 0,
				0xF, 0xE, 0xD, 0xC, 0xB, 0xA, 0x9, 0x8);

			const __m128i mask_step2 = _mm_set_epi8(
				0, 0, 0, 0, 0, 0, 0, 0,
				0, 0, 0, 0, 0x7, 0x6, 0x5, 0x4);

			const __m128i mask_step3 = _mm_set_epi8(
				0, 0, 0, 0, 0, 0, 0, 0,
				0, 0, 0, 0, 0, 0, 0x3, 0x2);

			__m128i tmp = _mm_shuffle_epi8(min, mask_step1);
			min = _mm_min_epu16(min, tmp);
			tmp = _mm_shuffle_epi8(min, mask_step2);
			min = _mm_min_epu16(min, tmp);
			tmp = _mm_shuffle_epi8(min, mask_step3);
			min = _mm_min_epu16(min, tmp);

			tmp = _mm_shuffle_epi8(max, mask_step1);
			max = _mm_max_epu16(max, tmp);
			tmp = _mm_shuffle_epi8(max, mask_step2);
			max = _mm_max_epu16(max, tmp);
			tmp = _mm_shuffle_epi8(max, mask_step3);
			max = _mm_max_epu16(max, tmp);

			const u16 min_index = u16(_mm_cvtsi128_si32(min) & 0xFFFF);
			const u16 max_index = u16(_mm_cvtsi128_si32(max) & 0xFFFF);

			return std::make_tuple(min_index, max_index, count);
		}

		static
		std::tuple<u32, u32, u32> upload_u32_swapped(const void *src, void *dst, u32 count)
		{
			const __m128i mask = _mm_set_epi8(
				0xC, 0xD, 0xE, 0xF,
				0x8, 0x9, 0xA, 0xB,
				0x4, 0x5, 0x6, 0x7,
				0x0, 0x1, 0x2, 0x3);

			auto src_stream = (const __m128i*)src;
			auto dst_stream = (__m128i*)dst;

			__m128i min = _mm_set1_epi32(~0u);
			__m128i max = _mm_set1_epi32(0);

			const auto iterations = count / 4;
			for (unsigned n = 0; n < iterations; ++n)
			{
				const __m128i raw = _mm_loadu_si128(src_stream++);
				const __m128i value = _mm_shuffle_epi8(raw, mask);
				max = _mm_max_epu32(max, value);
				min = _mm_min_epu32(min, value);
				_mm_storeu_si128(dst_stream++, value);
			}

			// Aggregate min-max
			const __m128i mask_step1 = _mm_set_epi8(
				0, 0, 0, 0, 0, 0, 0, 0,
				0xF, 0xE, 0xD, 0xC, 0xB, 0xA, 0x9, 0x8);

			const __m128i mask_step2 = _mm_set_epi8(
				0, 0, 0, 0, 0, 0, 0, 0,
				0, 0, 0, 0, 0x7, 0x6, 0x5, 0x4);

			__m128i tmp = _mm_shuffle_epi8(min, mask_step1);
			min = _mm_min_epu16(min, tmp);
			tmp = _mm_shuffle_epi8(min, mask_step2);
			min = _mm_min_epu16(min, tmp);

			tmp = _mm_shuffle_epi8(max, mask_step1);
			max = _mm_max_epu16(max, tmp);
			tmp = _mm_shuffle_epi8(max, mask_step2);
			max = _mm_max_epu16(max, tmp);

			const u32 min_index = u32(_mm_cvtsi128_si32(min));
			const u32 max_index = u32(_mm_cvtsi128_si32(max));

			return std::make_tuple(min_index, max_index, count);
		}

		template<typename T>
		static
		std::tuple<T, T, u32> upload_untouched(gsl::span<to_be_t<const T>> src, gsl::span<T> dst)
		{
			T min_index, max_index;
			u32 written;
			u32 remaining = src.size();

			if (s_use_ssse3 && remaining >= 32)
			{
				if constexpr (std::is_same<T, u32>::value)
				{
					const auto count = (remaining & ~0x3);
					std::tie(min_index, max_index, written) = upload_u32_swapped(src.data(), dst.data(), count);
				}
				else if constexpr (std::is_same<T, u16>::value)
				{
					const auto count = (remaining & ~0x7);
					std::tie(min_index, max_index, written) = upload_u16_swapped(src.data(), dst.data(), count);
				}

				remaining -= written;
			}
			else
			{
				min_index = index_limit<T>();
				max_index = 0;
				written = 0;
			}

			while (remaining--)
			{
				T index = src[written];
				dst[written++] = min_max(min_index, max_index, index);
			}

			return std::make_tuple(min_index, max_index, written);
		}
	};

	struct primitive_restart_impl
	{
		template<typename T>
		static
		std::tuple<T, T, u32> upload_untouched(gsl::span<to_be_t<const T>> src, gsl::span<T> dst, u32 restart_index, bool skip_restart)
		{
			T min_index = index_limit<T>(), max_index = 0;
			u32 dst_index = 0;

			for (const T index : src)
			{
				if (index == restart_index)
				{
					if (!skip_restart)
					{
						dst[dst_index++] = index_limit<T>();
					}
				}
				else
				{
					dst[dst_index++] = min_max(min_index, max_index, index);
				}
			}

			return std::make_tuple(min_index, max_index, dst_index);
		}
	};

	template<typename T>
	std::tuple<T, T, u32> upload_untouched(gsl::span<to_be_t<const T>> src, gsl::span<T> dst, rsx::primitive_type draw_mode, bool is_primitive_restart_enabled, u32 primitive_restart_index)
	{
		if (LIKELY(!is_primitive_restart_enabled))
		{
			return untouched_impl::upload_untouched(src, dst);
		}
		else
		{
			return primitive_restart_impl::upload_untouched(src, dst, primitive_restart_index, is_primitive_disjointed(draw_mode));
		}
	}

	template<typename T>
	std::tuple<T, T, u32> expand_indexed_triangle_fan(gsl::span<to_be_t<const T>> src, gsl::span<T> dst, bool is_primitive_restart_enabled, u32 primitive_restart_index)
	{
		const T invalid_index = index_limit<T>();

		T min_index = invalid_index;
		T max_index = 0;

		verify(HERE), (dst.size() >= 3 * (src.size() - 2));

		u32 dst_idx = 0;
		u32 src_idx = 0;

		bool needs_anchor = true;
		T anchor = invalid_index;
		T last_index = invalid_index;

		for (const T index : src)
		{
			if (needs_anchor)
			{
				if (is_primitive_restart_enabled && index == primitive_restart_index)
					continue;

				anchor = index;
				needs_anchor = false;
				continue;
			}

			if (is_primitive_restart_enabled && index == primitive_restart_index)
			{
				needs_anchor = true;
				last_index = invalid_index;
				continue;
			}

			if (last_index == invalid_index)
			{
				//Need at least one anchor and one outer index to create a triangle
				last_index = index;
				continue;
			}

			dst[dst_idx++] = anchor;
			dst[dst_idx++] = last_index;
			dst[dst_idx++] = min_max(min_index, max_index, index);

			last_index = index;
		}

		return std::make_tuple(min_index, max_index, dst_idx);
	}

	template<typename T>
	std::tuple<T, T, u32> expand_indexed_quads(gsl::span<to_be_t<const T>> src, gsl::span<T> dst, bool is_primitive_restart_enabled, u32 primitive_restart_index)
	{
		T min_index = index_limit<T>();
		T max_index = 0;

		verify(HERE), (4 * dst.size_bytes() >= 6 * src.size_bytes());

		u32 dst_idx = 0;
		u8 set_size = 0;
		T tmp_indices[4];

		for (const T index : src)
		{
			if (is_primitive_restart_enabled && index == primitive_restart_index)
			{
				//empty temp buffer
				set_size = 0;
				continue;
			}

			tmp_indices[set_size++] = min_max(min_index, max_index, index);

			if (set_size == 4)
			{
				// First triangle
				dst[dst_idx++] = tmp_indices[0];
				dst[dst_idx++] = tmp_indices[1];
				dst[dst_idx++] = tmp_indices[2];
				// Second triangle
				dst[dst_idx++] = tmp_indices[2];
				dst[dst_idx++] = tmp_indices[3];
				dst[dst_idx++] = tmp_indices[0];

				set_size = 0;
			}
		}

		return std::make_tuple(min_index, max_index, dst_idx);
	}
}

// Only handle quads and triangle fan now
bool is_primitive_native(rsx::primitive_type draw_mode)
{
	switch (draw_mode)
	{
	case rsx::primitive_type::points:
	case rsx::primitive_type::lines:
	case rsx::primitive_type::line_strip:
	case rsx::primitive_type::triangles:
	case rsx::primitive_type::triangle_strip:
	case rsx::primitive_type::quad_strip:
		return true;
	case rsx::primitive_type::line_loop:
	case rsx::primitive_type::polygon:
	case rsx::primitive_type::triangle_fan:
	case rsx::primitive_type::quads:
		return false;
	case rsx::primitive_type::invalid:
		break;
	}

	fmt::throw_exception("Wrong primitive type" HERE);
}

bool is_primitive_disjointed(rsx::primitive_type draw_mode)
{
	switch (draw_mode)
	{
	case rsx::primitive_type::line_loop:
	case rsx::primitive_type::line_strip:
	case rsx::primitive_type::polygon:
	case rsx::primitive_type::quad_strip:
	case rsx::primitive_type::triangle_fan:
	case rsx::primitive_type::triangle_strip:
		return false;
	default:
		return true;
	}
}

u32 get_index_count(rsx::primitive_type draw_mode, u32 initial_index_count)
{
	// Index count
	if (is_primitive_native(draw_mode))
		return initial_index_count;

	switch (draw_mode)
	{
	case rsx::primitive_type::line_loop:
		return initial_index_count + 1;
	case rsx::primitive_type::polygon:
	case rsx::primitive_type::triangle_fan:
		return (initial_index_count - 2) * 3;
	case rsx::primitive_type::quads:
		return (6 * initial_index_count) / 4;
	default:
		return 0;
	}
}

u32 get_index_type_size(rsx::index_array_type type)
{
	switch (type)
	{
	case rsx::index_array_type::u16: return sizeof(u16);
	case rsx::index_array_type::u32: return sizeof(u32);
	}
	fmt::throw_exception("Wrong index type" HERE);
}

void write_index_array_for_non_indexed_non_native_primitive_to_buffer(char* dst, rsx::primitive_type draw_mode, unsigned count)
{
	unsigned short *typedDst = (unsigned short *)(dst);
	switch (draw_mode)
	{
	case rsx::primitive_type::line_loop:
		for (unsigned i = 0; i < count; ++i)
			typedDst[i] = i;
		typedDst[count] = 0;
		return;
	case rsx::primitive_type::triangle_fan:
	case rsx::primitive_type::polygon:
		for (unsigned i = 0; i < (count - 2); i++)
		{
			typedDst[3 * i] = 0;
			typedDst[3 * i + 1] = i + 2 - 1;
			typedDst[3 * i + 2] = i + 2;
		}
		return;
	case rsx::primitive_type::quads:
		for (unsigned i = 0; i < count / 4; i++)
		{
			// First triangle
			typedDst[6 * i] = 4 * i;
			typedDst[6 * i + 1] = 4 * i + 1;
			typedDst[6 * i + 2] = 4 * i + 2;
			// Second triangle
			typedDst[6 * i + 3] = 4 * i + 2;
			typedDst[6 * i + 4] = 4 * i + 3;
			typedDst[6 * i + 5] = 4 * i;
		}
		return;
	case rsx::primitive_type::quad_strip:
	case rsx::primitive_type::points:
	case rsx::primitive_type::lines:
	case rsx::primitive_type::line_strip:
	case rsx::primitive_type::triangles:
	case rsx::primitive_type::triangle_strip:
		fmt::throw_exception("Native primitive type doesn't require expansion" HERE);
	case rsx::primitive_type::invalid:
		break;
	}

	fmt::throw_exception("Tried to load invalid primitive type" HERE);
}


namespace
{
	/**
	* Get first index and index count from a draw indexed clause.
	*/
	std::tuple<u32, u32> get_first_count_from_draw_indexed_clause(const std::vector<std::pair<u32, u32>>& first_count_arguments)
	{
		u32 first = std::get<0>(first_count_arguments.front());
		u32 count = std::get<0>(first_count_arguments.back()) + std::get<1>(first_count_arguments.back()) - first;
		return std::make_tuple(first, count);
	}

	template<typename T>
	std::tuple<T, T, u32> write_index_array_data_to_buffer_impl(gsl::span<T> dst,
		gsl::span<const be_t<T>> src,
		rsx::primitive_type draw_mode, bool restart_index_enabled, u32 restart_index,
		const std::function<bool(rsx::primitive_type)>& expands)
	{
		if (LIKELY(!expands(draw_mode)))
		{
			return upload_untouched<T>(src, dst, draw_mode, restart_index_enabled, restart_index);
		}

		switch (draw_mode)
		{
		case rsx::primitive_type::line_loop:
		{
			const auto &returnvalue = upload_untouched<T>(src, dst, draw_mode, restart_index_enabled, restart_index);
			const auto index_count = dst.size_bytes() / sizeof(T);
			dst[index_count] = src[0];
			return returnvalue;
		}
		case rsx::primitive_type::polygon:
		case rsx::primitive_type::triangle_fan:
		{
			return expand_indexed_triangle_fan<T>(src, dst, restart_index_enabled, restart_index);
		}
		case rsx::primitive_type::quads:
		{
			return expand_indexed_quads<T>(src, dst, restart_index_enabled, restart_index);
		}
		default:
			fmt::throw_exception("Unknown draw mode (0x%x)" HERE, (u32)draw_mode);
		}
	}
}

std::tuple<u32, u32, u32> write_index_array_data_to_buffer(gsl::span<gsl::byte> dst_ptr,
	gsl::span<const gsl::byte> src_ptr,
	rsx::index_array_type type, rsx::primitive_type draw_mode, bool restart_index_enabled, u32 restart_index,
	const std::function<bool(rsx::primitive_type)>& expands)
{
	switch (type)
	{
	case rsx::index_array_type::u16:
	{
		return write_index_array_data_to_buffer_impl<u16>(as_span_workaround<u16>(dst_ptr),
			as_const_span<const be_t<u16>>(src_ptr), draw_mode, restart_index_enabled, restart_index, expands);
	}
	case rsx::index_array_type::u32:
	{
		return write_index_array_data_to_buffer_impl<u32>(as_span_workaround<u32>(dst_ptr),
			as_const_span<const be_t<u32>>(src_ptr), draw_mode, restart_index_enabled, restart_index, expands);
	}
	default:
		fmt::throw_exception("Unreachable" HERE);
	}
}

void stream_vector(void *dst, u32 x, u32 y, u32 z, u32 w)
{
	__m128i vector = _mm_set_epi32(w, z, y, x);
	_mm_stream_si128((__m128i*)dst, vector);
}

void stream_vector(void *dst, f32 x, f32 y, f32 z, f32 w)
{
	stream_vector(dst, std::bit_cast<u32>(x), std::bit_cast<u32>(y), std::bit_cast<u32>(z), std::bit_cast<u32>(w));
}
void stream_vector_from_memory(void *dst, void *src)
{
	const __m128i &vector = _mm_loadu_si128((__m128i*)src);
	_mm_stream_si128((__m128i*)dst, vector);
}
