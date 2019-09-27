#ifndef CK_GRIDWISE_CONVOLUTION_IMPLICIT_GEMM_V4R4_NCHW_KCYX_NKHW_PADDED_HPP
#define CK_GRIDWISE_CONVOLUTION_IMPLICIT_GEMM_V4R4_NCHW_KCYX_NKHW_PADDED_HPP

#include "common_header.hpp"
#include "ConstantTensorDescriptor.hpp"
#include "ConstantMergedTensorDescriptor.hpp"
#include "ConstantMatrixDescriptor.hpp"
#include "blockwise_generic_tensor_slice_copy.hpp"
#include "blockwise_gemm.hpp"
#include "threadwise_generic_tensor_slice_copy.hpp"

namespace ck {

// B = merge(N, Ho, Wo)
template <index_t GridSize,
          index_t BlockSize,
          typename Float,
          typename InGlobalDesc,
          typename WeiGlobalDesc,
          typename OutGlobalDesc,
          typename ConvStrides,
          typename ConvDilations,
          typename LeftPads,
          typename RightPads,
          index_t BPerBlock,
          index_t KPerBlock,
          index_t EPerBlock,
          index_t GemmMPerThreadSubC,
          index_t GemmNPerThreadSubC,
          index_t GemmMLevel0Cluster,
          index_t GemmNLevel0Cluster,
          index_t GemmMLevel1Cluster,
          index_t GemmNLevel1Cluster,
          index_t GemmKPerThreadLoop,
          index_t GemmDataPerReadA,
          index_t GemmDataPerReadB,
          typename InBlockCopySubLengths_E_B,
          typename InBlockCopyClusterLengths_E_B,
          typename InBlockCopyThreadClusterArrangeOrder,
          typename InBlockCopySrcAccessOrder,
          typename InBlockCopyDstAccessOrder,
          index_t InBlockCopyDataPerAccess_B,
          typename WeiBlockCopySubLengths_E_K,
          typename WeiBlockCopyClusterLengths_E_K,
          typename WeiBlockCopyThreadClusterArrangeOrder,
          typename WeiBlockCopySrcAccessOrder,
          typename WeiBlockCopyDstAccessOrder,
          index_t WeiBlockCopySrcDataPerRead_E,
          index_t WeiBlockCopyDstDataPerWrite_K,
          index_t OutThreadCopyDataPerAccess_B>
struct GridwiseConvolutionImplicitGemm_v4r4_nchw_kcyx_nkhw_padded
{
#if 1
    __device__ void Run(const Float* const __restrict__ p_in_global,
                        const Float* const __restrict__ p_wei_global,
                        Float* const __restrict__ p_out_global) const
    {
        constexpr auto I0 = Number<0>{};
        constexpr auto I1 = Number<1>{};
        constexpr auto I2 = Number<2>{};
        constexpr auto I3 = Number<3>{};

        constexpr auto True = integral_constant<bool, true>{};

        constexpr auto in_n_c_hi_wi_global_desc =
            make_native_tensor_descriptor(InGlobalDesc::GetLengths(), InGlobalDesc::GetStrides());
        constexpr auto wei_k_c_y_x_global_desc =
            make_native_tensor_descriptor(WeiGlobalDesc::GetLengths(), WeiGlobalDesc::GetStrides());
        constexpr auto out_n_k_ho_wo_global_desc =
            make_native_tensor_descriptor(OutGlobalDesc::GetLengths(), OutGlobalDesc::GetStrides());

        constexpr index_t N  = in_n_c_hi_wi_global_desc.GetLength(I0);
        constexpr index_t C  = in_n_c_hi_wi_global_desc.GetLength(I1);
        constexpr index_t Hi = in_n_c_hi_wi_global_desc.GetLength(I2);
        constexpr index_t Wi = in_n_c_hi_wi_global_desc.GetLength(I3);

        constexpr index_t K  = out_n_k_ho_wo_global_desc.GetLength(I1);
        constexpr index_t Ho = out_n_k_ho_wo_global_desc.GetLength(I2);
        constexpr index_t Wo = out_n_k_ho_wo_global_desc.GetLength(I3);

        constexpr index_t Y = wei_k_c_y_x_global_desc.GetLength(I2);
        constexpr index_t X = wei_k_c_y_x_global_desc.GetLength(I3);

        constexpr index_t ConvStrideH = ConvStrides{}[0];
        constexpr index_t ConvStrideW = ConvStrides{}[1];

        constexpr index_t ConvDilationH = ConvDilations{}[0];
        constexpr index_t ConvDilationW = ConvDilations{}[1];

        constexpr index_t E = C * Y * X;
        constexpr index_t B = N * Ho * Wo;

        // sanity-check for vectorized memory load
        static_assert((Wo == 1 || (ConvStrideW == 1 || InBlockCopyDataPerAccess_B == 1)) &&
                          (X == 1 || ConvDilationW % InBlockCopyDataPerAccess_B == 0),
                      "wrong! aligment requirement for vectorized global load of input tensor will "
                      "be violated");

        // divide block work by [K, B]
        static_assert(K % KPerBlock == 0 && B % BPerBlock == 0 && E % EPerBlock == 0,
                      "wrong! cannot divide work evenly among block");

        constexpr index_t KBlockWork = K / KPerBlock;
        constexpr index_t BBlockWork = B / BPerBlock;

        constexpr auto block_work_desc =
            make_ConstantTensorDescriptor_packed(Sequence<KBlockWork, BBlockWork>{});

        const auto block_work_multi_id =
            block_work_desc.GetMultiIndexFrom1dIndex(get_block_1d_id());

        const index_t k_block_data_on_global = block_work_multi_id[0] * KPerBlock;
        const index_t b_block_data_on_global = block_work_multi_id[1] * BPerBlock;

        // input tensor
        //   global mem
        constexpr auto in_n_c_hip_wip_global_desc = transform_tensor_descriptor(
            in_n_c_hi_wi_global_desc,
            make_tuple(
                PassThrough<N>{}, PassThrough<C>{}, Pad<Sequence<Hi, Wi>, LeftPads, RightPads>{}),
            make_tuple(Sequence<0>{}, Sequence<1>{}, Sequence<2, 3>{}),
            make_tuple(Sequence<0>{}, Sequence<1>{}, Sequence<2, 3>{}));

        constexpr auto in_n_c_y_ho_x_wo_global_desc = transform_tensor_descriptor(
            in_n_c_hip_wip_global_desc,
            make_tuple(PassThrough<N>{},
                       PassThrough<C>{},
                       Embed<Sequence<Y, Ho>, Sequence<ConvDilationH, ConvStrideH, 0>>{},
                       Embed<Sequence<X, Wo>, Sequence<ConvDilationW, ConvStrideW, 0>>{}),
            make_tuple(Sequence<0>{}, Sequence<1>{}, Sequence<2>{}, Sequence<3>{}),
            make_tuple(Sequence<0>{}, Sequence<1>{}, Sequence<2, 3>{}, Sequence<4, 5>{}));

        constexpr auto in_e_b_global_desc = transform_tensor_descriptor(
            in_n_c_y_ho_x_wo_global_desc,
            make_tuple(Merge<Sequence<C, Y, X>>{}, Merge<Sequence<N, Ho, Wo>>{}),
            make_tuple(Sequence<1, 2, 4>{}, Sequence<0, 3, 5>{}),
            make_tuple(Sequence<0>{}, Sequence<1>{}));

        //   LDS mem
        //     be careful of LDS alignment
        constexpr auto in_e_b_block_desc =
            make_native_tensor_descriptor_packed(Sequence<EPerBlock, BPerBlock>{});

        // input blockwise copy
        auto blockwise_in_copy =
            BlockwiseGenericTensorSliceCopy_v4<BlockSize,
                                               decltype(in_e_b_global_desc),
                                               decltype(in_e_b_block_desc),
                                               decltype(in_e_b_block_desc.GetLengths()),
                                               InBlockCopySubLengths_E_B,
                                               InBlockCopyClusterLengths_E_B,
                                               InBlockCopyThreadClusterArrangeOrder,
                                               InBlockCopySrcAccessOrder,
                                               InBlockCopyDstAccessOrder,
                                               1,
                                               1,
                                               InBlockCopyDataPerAccess_B,
                                               InBlockCopyDataPerAccess_B>(
                {0, b_block_data_on_global}, {0, 0});

        // weight tensor
        //   global mem
        constexpr auto wei_e_k_global_desc =
            transform_tensor_descriptor(wei_k_c_y_x_global_desc,
                                        make_tuple(Merge<Sequence<C, Y, X>>{}, PassThrough<K>{}),
                                        make_tuple(Sequence<1, 2, 3>{}, Sequence<0>{}),
                                        make_tuple(Sequence<0>{}, Sequence<1>{}));

        //   LDS
        //     be careful of LDS alignment
        constexpr auto wei_e_k_block_desc = make_native_tensor_descriptor_aligned(
            Sequence<EPerBlock, KPerBlock>{},
            Number<math::lcm(WeiBlockCopyDstDataPerWrite_K, GemmDataPerReadA)>{});

        //     this check is ad-hoc
        //     TODO: need to properly implement tensor descriptor with multiple alignment
        //     requirements
        static_assert(wei_e_k_block_desc.GetStride(I0) % GemmDataPerReadA == 0,
                      "GemmDataPerReadA alignment requirement is not satisfied");

        // weight blockwise copy
        auto blockwise_wei_copy =
            BlockwiseGenericTensorSliceCopy_v4<BlockSize,
                                               decltype(wei_e_k_global_desc),
                                               decltype(wei_e_k_block_desc),
                                               decltype(wei_e_k_block_desc.GetLengths()),
                                               WeiBlockCopySubLengths_E_K,
                                               WeiBlockCopyClusterLengths_E_K,
                                               WeiBlockCopyThreadClusterArrangeOrder,
                                               WeiBlockCopySrcAccessOrder,
                                               WeiBlockCopyDstAccessOrder,
                                               0,
                                               1,
                                               WeiBlockCopySrcDataPerRead_E,
                                               WeiBlockCopyDstDataPerWrite_K>(
                {0, k_block_data_on_global}, {0, 0});

        // GEMM definition
        // c_mtx += transpose(a_mtx) * b_mtx
        //     a_mtx[EPerBlock, KPerBlock] is in LDS
        //     b_mtx[EPerBlocl, BPerBlock] is in LDS
        //     c_mtx[KPerBlock, BPerBlock] is distributed among threads, and saved in
        //     register
        constexpr auto a_e_k_block_mtx_desc = make_ConstantMatrixDescriptor(wei_e_k_block_desc);

        constexpr auto b_e_b_block_mtx_desc = make_ConstantMatrixDescriptor(in_e_b_block_desc);

        // sanity check
        static_assert(
            KPerBlock % (GemmMPerThreadSubC * GemmMLevel0Cluster * GemmMLevel1Cluster) == 0 &&
                BPerBlock % (GemmNPerThreadSubC * GemmNLevel0Cluster * GemmNLevel1Cluster) == 0,
            "wrong!");

        constexpr index_t GemmMRepeat =
            KPerBlock / (GemmMPerThreadSubC * GemmMLevel0Cluster * GemmMLevel1Cluster);

        constexpr index_t GemmNRepeat =
            BPerBlock / (GemmNPerThreadSubC * GemmNLevel0Cluster * GemmNLevel1Cluster);

        // c_thread_mtx definition: this is a mess
        // TODO:: more elegent way of defining c_thread_mtx
        constexpr auto c_k0k1_b0b1_thread_mtx_desc = make_ConstantMatrixDescriptor_packed(
            Number<GemmMRepeat * GemmMPerThreadSubC>{}, Number<GemmNRepeat * GemmNPerThreadSubC>{});

        const auto blockwise_gemm = BlockwiseGemmBlockABlockBThreadCTransANormalBNormalC_v2<
            BlockSize,
            decltype(a_e_k_block_mtx_desc),
            decltype(b_e_b_block_mtx_desc),
            decltype(c_k0k1_b0b1_thread_mtx_desc),
            GemmMPerThreadSubC,
            GemmNPerThreadSubC,
            GemmMLevel0Cluster,
            GemmNLevel0Cluster,
            GemmMLevel1Cluster,
            GemmNLevel1Cluster,
            GemmKPerThreadLoop,
            GemmDataPerReadA,
            GemmDataPerReadB>{};

        // LDS allocation for input and weight: be careful of alignment
        constexpr index_t max_align = math::lcm(InBlockCopyDataPerAccess_B,
                                                WeiBlockCopyDstDataPerWrite_K,
                                                GemmDataPerReadA,
                                                GemmDataPerReadB);

        constexpr index_t in_block_space =
            math::integer_least_multiple(in_e_b_block_desc.GetElementSpace(), max_align);

        constexpr index_t wei_block_space =
            math::integer_least_multiple(wei_e_k_block_desc.GetElementSpace(), max_align);

        __shared__ Float p_in_block[in_block_space];
        __shared__ Float p_wei_block[wei_block_space];

        // register allocation for output
        Float p_out_thread[c_k0k1_b0b1_thread_mtx_desc.GetElementSpace()];

        // zero out threadwise output
        threadwise_matrix_set_zero(c_k0k1_b0b1_thread_mtx_desc, p_out_thread);

        for(index_t e_block_data_begin = 0; e_block_data_begin < E; e_block_data_begin += EPerBlock)
        {
            blockwise_in_copy.Run(p_in_global, p_in_block);
            blockwise_wei_copy.Run(p_wei_global, p_wei_block);

            __syncthreads();

            blockwise_gemm.Run(p_wei_block, p_in_block, p_out_thread);

            __syncthreads();

            blockwise_in_copy.MoveSrcSliceWindow(make_multi_index(EPerBlock, 0), True);
            blockwise_wei_copy.MoveSrcSliceWindow(make_multi_index(EPerBlock, 0), True);
        }

        // copy output: register to global memory
        {
            // calculate origin of thread output tensor on global memory
            //     blockwise GEMM c matrix starting index
            const auto c_thread_mtx_on_block =
                blockwise_gemm.GetBeginOfThreadMatrixC(get_thread_local_1d_id());

            const index_t k_thread_data_on_global =
                k_block_data_on_global + c_thread_mtx_on_block.row;

            const index_t b_thread_data_on_global =
                b_block_data_on_global + c_thread_mtx_on_block.col;

            // src descriptor
            constexpr auto out_k0_k1_b0_b1_thread_desc = make_native_tensor_descriptor_packed(
                Sequence<GemmMRepeat, GemmMPerThreadSubC, GemmNRepeat, GemmNPerThreadSubC>{});

            // dst descriptor
            constexpr index_t K1 = GemmMPerThreadSubC * GemmMLevel0Cluster * GemmMLevel1Cluster;
            constexpr index_t B1 = GemmNPerThreadSubC * GemmNLevel0Cluster * GemmNLevel1Cluster;

            constexpr index_t K0 = K / K1;
            constexpr index_t B0 = B / B1;

            constexpr auto out_k_b_global_desc = transform_tensor_descriptor(
                out_n_k_ho_wo_global_desc,
                make_tuple(PassThrough<K>{}, Merge<Sequence<N, Ho, Wo>>{}),
                make_tuple(Sequence<1>{}, Sequence<0, 2, 3>{}),
                make_tuple(Sequence<0>{}, Sequence<1>{}));

            constexpr auto out_k0_k1_b0_b1_global_desc = transform_tensor_descriptor(
                out_k_b_global_desc,
                make_tuple(Unmerge<Sequence<K0, K1>>{}, Unmerge<Sequence<B0, B1>>{}),
                make_tuple(Sequence<0>{}, Sequence<1>{}),
                make_tuple(Sequence<0, 1>{}, Sequence<2, 3>{}));

            // output threadwise copy
            auto threadwise_out_copy = ThreadwiseGenericTensorSliceCopy_v4r2<
                decltype(out_k0_k1_b0_b1_thread_desc),
                decltype(out_k0_k1_b0_b1_global_desc),
                decltype(out_k0_k1_b0_b1_thread_desc.GetLengths()),
                arithmetic_sequence_gen<0, 4, 1>::type,
                3,
                OutThreadCopyDataPerAccess_B,
                OutThreadCopyDataPerAccess_B>({0, 0, 0, 0},
                                              {k_thread_data_on_global / K1,
                                               k_thread_data_on_global % K1,
                                               b_thread_data_on_global / B1,
                                               b_thread_data_on_global % B1});

            threadwise_out_copy.Run(p_out_thread, p_out_global);
        }
    }
#else
    __device__ void Run(const Float* const __restrict__ p_in_global,
                        const Float* const __restrict__ p_wei_global,
                        Float* const __restrict__ p_out_global) const
    {
        constexpr auto I0 = Number<0>{};
        constexpr auto I1 = Number<1>{};
        constexpr auto I2 = Number<2>{};
        constexpr auto I3 = Number<3>{};

        constexpr auto True = integral_constant<bool, true>{};

        constexpr auto in_n_c_hi_wi_global_desc =
            make_native_tensor_descriptor(InGlobalDesc::GetLengths(), InGlobalDesc::GetStrides());
        constexpr auto wei_k_c_y_x_global_desc =
            make_native_tensor_descriptor(WeiGlobalDesc::GetLengths(), WeiGlobalDesc::GetStrides());
        constexpr auto out_n_k_ho_wo_global_desc =
            make_native_tensor_descriptor(OutGlobalDesc::GetLengths(), OutGlobalDesc::GetStrides());

        constexpr index_t N  = in_n_c_hi_wi_global_desc.GetLength(I0);
        constexpr index_t C  = in_n_c_hi_wi_global_desc.GetLength(I1);
        constexpr index_t Hi = in_n_c_hi_wi_global_desc.GetLength(I2);
        constexpr index_t Wi = in_n_c_hi_wi_global_desc.GetLength(I3);

        constexpr index_t K  = out_n_k_ho_wo_global_desc.GetLength(I1);
        constexpr index_t Ho = out_n_k_ho_wo_global_desc.GetLength(I2);
        constexpr index_t Wo = out_n_k_ho_wo_global_desc.GetLength(I3);

        constexpr index_t Y = wei_k_c_y_x_global_desc.GetLength(I2);
        constexpr index_t X = wei_k_c_y_x_global_desc.GetLength(I3);

        constexpr index_t ConvStrideH = ConvStrides{}[0];
        constexpr index_t ConvStrideW = ConvStrides{}[1];

        constexpr index_t ConvDilationH = ConvDilations{}[0];
        constexpr index_t ConvDilationW = ConvDilations{}[1];

        constexpr index_t E = C * Y * X;
        constexpr index_t B = N * Ho * Wo;

        // sanity-check for vectorized memory load
        static_assert((Ho == 1 || ConvStrideW % InBlockCopyDataPerAccess_B == 0) &&
                          (X == 1 || ConvDilationW % InBlockCopyDataPerAccess_B == 0),
                      "wrong! aligment requirement for vectorized global load of input tensor will "
                      "be violated");

        // input tensor
        constexpr auto in_n_c_hip_wip_global_desc = transform_tensor_descriptor(
            in_n_c_hi_wi_global_desc,
            make_tuple(
                PassThrough<N>{}, PassThrough<C>{}, Pad<Sequence<Hi, Wi>, LeftPads, RightPads>{}),
            make_tuple(Sequence<0>{}, Sequence<1>{}, Sequence<2, 3>{}),
            make_tuple(Sequence<0>{}, Sequence<1>{}, Sequence<2, 3>{}));

        constexpr auto in_n_c_y_ho_x_wo_global_desc = transform_tensor_descriptor(
            in_n_c_hip_wip_global_desc,
            make_tuple(PassThrough<N>{},
                       PassThrough<C>{},
                       Embed<Sequence<Y, Ho>, Sequence<ConvDilationH, ConvStrideH, 0>>{},
                       Embed<Sequence<X, Wo>, Sequence<ConvDilationW, ConvStrideW, 0>>{}),
            make_tuple(Sequence<0>{}, Sequence<1>{}, Sequence<2>{}, Sequence<3>{}),
            make_tuple(Sequence<0>{}, Sequence<1>{}, Sequence<2, 3>{}, Sequence<4, 5>{}));

        constexpr auto in_e_b_global_desc = transform_tensor_descriptor(
            in_n_c_y_ho_x_wo_global_desc,
            make_tuple(Merge<Sequence<C, Y, X>>{}, Merge<Sequence<N, Ho, Wo>>{}),
            make_tuple(Sequence<1, 2, 4>{}, Sequence<0, 3, 5>{}),
            make_tuple(Sequence<0>{}, Sequence<1>{}));

        // output tensor
        constexpr auto out_k_b_global_desc =
            transform_tensor_descriptor(out_n_k_ho_wo_global_desc,
                                        make_tuple(PassThrough<K>{}, Merge<Sequence<N, Ho, Wo>>{}),
                                        make_tuple(Sequence<1>{}, Sequence<0, 2, 3>{}),
                                        make_tuple(Sequence<0>{}, Sequence<1>{}));

        constexpr index_t K1 = GemmMPerThreadSubC * GemmMLevel0Cluster * GemmMLevel1Cluster;
        constexpr index_t B1 = GemmNPerThreadSubC * GemmNLevel0Cluster * GemmNLevel1Cluster;

        constexpr index_t K0 = K / K1;
        constexpr index_t B0 = B / B1;

        constexpr auto out_k0_k1_b0_b1_global_desc = transform_tensor_descriptor(
            out_k_b_global_desc,
            make_tuple(Unmerge<Sequence<K0, K1>>{}, Unmerge<Sequence<B0, B1>>{}),
            make_tuple(Sequence<0>{}, Sequence<1>{}),
            make_tuple(Sequence<0, 1>{}, Sequence<2, 3>{}));

#if 1
        if(get_thread_local_1d_id() == 0 && get_block_1d_id() == 0)
        {
            print_tensor_descriptor("in_e_b_global_desc: ", in_e_b_global_desc);
            print_tensor_descriptor("in_n_c_y_ho_x_wo_global_desc: ", in_n_c_y_ho_x_wo_global_desc);
            print_tensor_descriptor("in_n_c_hip_wip_global_desc: ", in_n_c_hip_wip_global_desc);
            print_tensor_descriptor("in_n_c_hi_wi_global_desc: ", in_n_c_hi_wi_global_desc);

            auto coord3 = make_tensor_coordinate_v2(in_e_b_global_desc, {1, 1});

            auto idx3 = coord3.GetIndex();
            auto idx2 = coord3.GetLowerCoordinate().GetIndex();
            auto idx1 = coord3.GetLowerCoordinate().GetLowerCoordinate().GetIndex();
            auto idx0 =
                coord3.GetLowerCoordinate().GetLowerCoordinate().GetLowerCoordinate().GetIndex();

            print_array("idx3: ", idx3);
            print_array("idx2: ", idx2);
            print_array("idx1: ", idx1);
            print_array("idx0: ", idx0);
        }

        if(get_thread_local_1d_id() == 0 && get_block_1d_id() == 0)
        {
            print_tensor_descriptor("out_k0_k1_b0_b1_global_desc: ", out_k0_k1_b0_b1_global_desc);
            print_tensor_descriptor("out_k_b_global_desc: ", out_k_b_global_desc);
            print_tensor_descriptor("out_n_k_ho_wo_global_desc: ", out_n_k_ho_wo_global_desc);

            auto coord2 = make_tensor_coordinate_v2(out_k0_k1_b0_b1_global_desc, {1, 1, 1, 1});

            auto idx2 = coord2.GetIndex();
            auto idx1 = coord2.GetLowerCoordinate().GetIndex();
            auto idx0 = coord2.GetLowerCoordinate().GetLowerCoordinate().GetIndex();

            print_array("idx2: ", idx2);
            print_array("idx1: ", idx1);
            print_array("idx0: ", idx0);
        }
#endif
    }
#endif
};

} // namespace ck
#endif