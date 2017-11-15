/* ============================================================================
# [2017] - Robik AI Ltd - Paul Balanca
# All Rights Reserved.

# NOTICE: All information contained herein is, and remains
# the property of Robik AI Ltd, and its suppliers
# if any.  The intellectual and technical concepts contained
# herein are proprietary to Robik AI Ltd
# and its suppliers and may be covered by U.S., European and Foreign Patents,
# patents in process, and are protected by trade secret or copyright law.
# Dissemination of this information or reproduction of this material
# is strictly forbidden unless prior written permission is obtained
# from Robik AI Ltd.
# =========================================================================== */
#ifndef TFRT_NASNET_H
#define TFRT_NASNET_H

#include <map>

#include <NvInfer.h>
#include "../tensorflowrt.h"

namespace nasnet
{
/** Arg scope for NASNet: SAME padding + batch normalization. ReLU before.
 */
typedef tfrt::separable_convolution2d<tfrt::ActivationType::NONE, tfrt::PaddingType::SAME, true> separable_conv2d;
typedef tfrt::convolution2d<tfrt::ActivationType::NONE, tfrt::PaddingType::SAME, true>  conv2d;
typedef tfrt::convolution2d<tfrt::ActivationType::NONE, tfrt::PaddingType::VALID, true>  conv2d_valid;
        
typedef tfrt::max_pooling2d<tfrt::PaddingType::SAME>    max_pool2d;
typedef tfrt::avg_pooling2d<tfrt::PaddingType::SAME>    avg_pool2d;
typedef tfrt::concat_channels                           concat_channels;

/* ============================================================================
 * NASNet Abstract cell
 * ========================================================================== */
class base_cell
{
public:
    base_cell(tfrt::scope sc, size_t num_filters, float filter_scaling) :
        m_scope{sc}, m_num_conv_filters{num_filters}, m_filter_scaling{filter_scaling}
    {
    }
    /** Base of every cell: 1x1 convolution. */
    nvinfer1::ITensor* base_cell_n(nvinfer1::ITensor* net, tfrt::scope sc) const
    {
        size_t fsize = this->filter_size();
        // ReLU + conv.
        net = tfrt::relu(sc)(net);
        net = conv2d(sc, "1x1").noutputs(fsize).ksize(1)(net);
        return net;
    }
    /** Reduce n-1 layer to correct shape. */
    nvinfer1::ITensor* reduce_n_1(
        nvinfer1::ITensor* net_n, nvinfer1::ITensor* net_n_1, tfrt::scope sc) const
    {
        nvinfer1::ITensor* net = net_n_1;
        // No previous layer?
        if (net_n_1 == nullptr) {
            return net_n;
        }
        size_t fsize = this->filter_size();
        auto shape_n = tfrt::dims_nchw(net_n);
        auto shape_n_1 = tfrt::dims_nchw(net_n_1);
        // Approximation of the original implementation. TODO: FIX stride=2
        // First case: different HW shape.
        if (shape_n.h() != shape_n_1.w() || shape_n.h() != shape_n_1.w()) {
            net = avg_pool2d(sc).ksize(3).stride(2)(net);
        }
        // Number of channels different?
        if (int(fsize) != shape_n_1.c()) {
            net = conv2d(sc, "1x1").noutputs(fsize).ksize(1)(net);
        }
        return net;
    }
    /** Identity block: reduce to number of outputs, or stride > 1. */
    nvinfer1::ITensor* identity(nvinfer1::ITensor* net, tfrt::scope sc,
        size_t stride, size_t num_outputs, bool relu=false) const
    {
        auto shape = tfrt::dims_nchw(net);
        if (stride > 1 || shape.c() != int(num_outputs)) {
            if (relu) {
                net = tfrt::relu(sc)(net);
            }
            net = conv2d(sc, "1x1").noutputs(num_outputs).ksize(1).stride(1)(net);
        }
        return net;
    }
    /** Stack of separable convolutions. */
    nvinfer1::ITensor* sep_conv2d_stacked(nvinfer1::ITensor* net, tfrt::scope sc,
        size_t ksize, size_t stride, size_t num_outputs, size_t num_layers) const
    {
        std::ostringstream ostr;
        for (size_t i = 0 ; i < num_layers ; ++i) {
            // Scope...
            ostr.str("separable_");
            ostr << ksize << "x" << ksize << "_" << (i+1);
            // ReLU + Separable conv.
            net = tfrt::relu(sc)(net);
            net = separable_conv2d(sc, ostr.str()).noutputs(num_outputs).ksize(ksize)(net);
        }
        return net;
    }
public:
    size_t filter_size() const {
        return size_t(m_num_conv_filters * m_filter_scaling);
    }

protected:
    /** General scope of the cell. */
    tfrt::scope  m_scope;
    /** Number of convolution filters, normalized. */
    size_t  m_num_conv_filters;
    /** Filter scaling, used to compute the final number of filters. */
    float  m_filter_scaling;
};

/* ============================================================================
 * NASNet Normal cell
 * ========================================================================== */
class normal_cell : public base_cell
{
public:
    normal_cell(tfrt::scope sc, size_t num_filters, float filter_scaling) : 
        base_cell(sc, num_filters, filter_scaling)
    {
    }
    /** Operator: taking layers n and n-1. */
    nvinfer1::ITensor* operator()(nvinfer1::ITensor* net_n, nvinfer1::ITensor* net_n_1)
    {
        nvinfer1::ITensor* net_l, *net_r, *net;
        std::vector<nvinfer1::ITensor*> blocks;
        tfrt::scope sc{m_scope};
        // Basic cell + reduce n-1.
        net_n = this->base_cell_n(net_n, m_scope.sub("base_cell_n"));
        net_n_1 = this->reduce_n_1(net_n, net_n_1, m_scope.sub("reduce_n_1"));
        size_t fsize = this->filter_size();

        // Block 1: sep. 5x5 + sep 3x3 (n / n-1).
        sc = m_scope.sub("comb_iter_1");
        net_l = this->sep_conv2d_stacked(net_n, sc, 5, 1, fsize, 2);
        net_r = this->sep_conv2d_stacked(net_n_1, sc, 3, 1, fsize, 2);
        blocks.push_back( tfrt::add(sc)(net_l, net_r) );

        // Block 2: sep. 5x5 + sep 3x3 (n-1 / n-1).
        sc = m_scope.sub("comb_iter_2");
        net_l = this->sep_conv2d_stacked(net_n_1, sc, 5, 1, fsize, 2);
        net_r = this->sep_conv2d_stacked(net_n_1, sc, 3, 1, fsize, 2);
        blocks.push_back( tfrt::add(sc)(net_l, net_r) );
        
        // Block 3: avg_pool_3x3 + id (n / n-1).
        sc = m_scope.sub("comb_iter_3");
        net_l = avg_pool2d(sc).ksize(3)(net_n);
        net_l = this->identity(net_l, sc, 1, fsize, false);
        net_r = this->identity(net_n_1, sc, 1, fsize, true);  
        blocks.push_back( tfrt::add(sc)(net_l, net_r) );
        
        // Block 4: avg_pool_3x3 + avg_pool_3x3 (n-1 / n-1).
        sc = m_scope.sub("comb_iter_4");
        net_l = avg_pool2d(sc).ksize(3)(net_n_1);
        net_l = this->identity(net_l, sc, 1, fsize, false);
        net_r = avg_pool2d(sc).ksize(3)(net_n_1);
        net_r = this->identity(net_r, sc, 1, fsize, false);  
        blocks.push_back( tfrt::add(sc)(net_l, net_r) );

        // Block 5: separable_3x3_2 + id (n / n).
        sc = m_scope.sub("comb_iter_5");
        net_l = this->sep_conv2d_stacked(net_n, sc, 3, 1, fsize, 2);
        net_r = this->identity(net_n, sc, 1, fsize, true);
        blocks.push_back( tfrt::add(sc)(net_l, net_r) );

        // Concat this big mess!
        net = tfrt::concat_channels(sc)(blocks);
        return net;
    }
};

/* ============================================================================
 * NASNet Reduction cell
 * ========================================================================== */
class reduction_cell : public base_cell
{
public:
    reduction_cell(tfrt::scope sc, size_t num_filters, float filter_scaling) : 
        base_cell(sc, num_filters, filter_scaling)
    {
    }
    /** Operator: taking layers n and n-1. */
    nvinfer1::ITensor* operator()(nvinfer1::ITensor* net_n, nvinfer1::ITensor* net_n_1)
    {
        nvinfer1::ITensor* net_l, *net_r, *net;
        std::vector<nvinfer1::ITensor*> blocks, blocks_tmp;
        tfrt::scope sc{m_scope};
        // Basic cell + reduce n-1.
        net_n = this->base_cell_n(net_n, m_scope.sub("base_cell_n"));
        net_n_1 = this->reduce_n_1(net_n, net_n_1, m_scope.sub("reduce_n_1"));
        size_t fsize = this->filter_size();

        // Block 1: sep. 5x5 + sep 7x7 (n / n-1).
        sc = m_scope.sub("comb_iter_1");
        net_l = this->sep_conv2d_stacked(net_n, sc, 5, 2, fsize, 2);
        net_r = this->sep_conv2d_stacked(net_n_1, sc, 7, 2, fsize, 2);
        blocks_tmp.push_back( tfrt::add(sc)(net_l, net_r) );

        // Block 2: max_pool_3x3 + separable_7x7_2 (n / n-1).
        sc = m_scope.sub("comb_iter_2");
        net_l = max_pool2d(sc).ksize(3).stride(2)(net_n);
        net_l = this->identity(net_l, sc, 1, fsize, false);
        net_r = this->sep_conv2d_stacked(net_n_1, sc, 7, 2, fsize, 2);
        blocks_tmp.push_back( tfrt::add(sc)(net_l, net_r) );
        
        // Block 3: avg_pool_3x3 + separable_5x5_2 (n / n-1).
        sc = m_scope.sub("comb_iter_3");
        net_l = avg_pool2d(sc).ksize(3).stride(2)(net_n);
        net_l = this->identity(net_l, sc, 1, fsize, false);
        net_r = this->sep_conv2d_stacked(net_n_1, sc, 5, 2, fsize, 2);
        blocks.push_back( tfrt::add(sc)(net_l, net_r) );
        
        // Block 4: id + avg_pool_3x3 (tmp / tmp).
        sc = m_scope.sub("comb_iter_4");
        net_l = this->identity(blocks_tmp[1], sc, 1, fsize, false);
        net_r = avg_pool2d(sc).ksize(3)(blocks_tmp[0]);
        net_r = this->identity(net_r, sc, 1, fsize, false);  
        blocks.push_back( tfrt::add(sc)(net_l, net_r) );

        // Block 5: separable_3x3_2 + max_pool_3x3 (tmp / n).
        sc = m_scope.sub("comb_iter_5");
        net_l = this->sep_conv2d_stacked(blocks_tmp[0], sc, 3, 1, fsize, 2);
        net_r = max_pool2d(sc).ksize(3).stride(2)(net_n);
        blocks.push_back( tfrt::add(sc)(net_l, net_r) );

        // Concat this big mess!
        net = tfrt::concat_channels(sc)(blocks);
        return net;
    }
};

/* ============================================================================
 * NASNet network.
 * ========================================================================== */
class net : public tfrt::imagenet_network
{
public:
    net(std::string name, 
        float stem_multiplier, float filter_scaling_rate,
        size_t num_cells, size_t num_reduction_layers, size_t num_conv_filters) : 
            tfrt::imagenet_network(name, 1000, true),
            m_stem_multiplier{stem_multiplier}, 
            m_filter_scaling_rate{filter_scaling_rate},
            m_num_cells{num_cells},
            m_num_reduction_layers{num_reduction_layers},
            m_num_conv_filters{num_conv_filters}
    {
    }
    /** Build the network. Whohohoooo! */
    virtual nvinfer1::ITensor* build(tfrt::scope sc) {
        auto net = tfrt::input(sc)();
        
        // ImageNet stem cells.
        auto cell_outputs = imagenet_stem(net, sc);

        // Core of the network.
        auto reduction_cells = this->reduction_layers();
        double filter_scaling = 1.0;
        size_t true_cell_num = 2;
        for (size_t i = 0 ; i < m_num_cells ; ++i) {
            // Reduction cell?
            if (reduction_cells[i]) {
                filter_scaling *= m_filter_scaling_rate;
                // Scope name + cell construction...
                std::ostringstream ostr("reduction_cell_");   ostr << i;
                auto ssc = sc.sub(ostr.str());
                auto cell = reduction_cell(ssc, m_num_conv_filters, filter_scaling);
                // Apply!
                int n = cell_outputs.size();
                net = cell(cell_outputs[n-1], cell_outputs[n-2]);
                cell_outputs.push_back(net);
                true_cell_num++;
            }

            // Scope name + normal cell construction...
            std::ostringstream ostr("cell_");   ostr << i;
            auto ssc = sc.sub(ostr.str());
            auto cell = normal_cell(ssc, m_num_conv_filters, filter_scaling);
            // Apply!
            int n = cell_outputs.size();
            net = cell(cell_outputs[n-1], cell_outputs[n-2]);
            cell_outputs.push_back(net);
            true_cell_num++;
        }
        // Logits?
        // auto ssc = sc.sub("Logits");
        // net = avg_pool2d(ssc, "AvgPool_1a_7x7").ksize({7, 7})(net);
        // net = conv2d(ssc, "Conv2d_1c_1x1").noutputs(num_classes).ksize({1, 1})(net);
        net = tfrt::softmax(sc, "Softmax")(net);
        return net;
    }

protected:
    /** ImageNet stem: 3x3 conv + reduction cells. */
    std::vector<nvinfer1::ITensor*> imagenet_stem(nvinfer1::ITensor* net, tfrt::scope sc) const {
        // First 3x3 convolution.
        size_t num_stem_filters = int(32 * m_stem_multiplier);
        // VALID padding?
        net = conv2d_valid(sc, "conv0").ksize(3).stride(2).noutputs(num_stem_filters)(net);

        // Reduction cells.
        size_t num_stem_cells = 2;
        std::vector<nvinfer1::ITensor*> cell_outputs;
        cell_outputs.push_back(nullptr);
        cell_outputs.push_back(net);

        // Filter scaling?
        double filter_scaling = 1.0 / pow(double(m_filter_scaling_rate), double(num_stem_cells));
        for (size_t i = 0 ; i < num_stem_cells ; ++i) {
            // Scope name + cell construction...
            std::ostringstream ostr("cell_stem_");   ostr << i;
            auto ssc = sc.sub(ostr.str());
            auto cell = reduction_cell(ssc, m_num_conv_filters, filter_scaling);

            // Apply!
            int n = cell_outputs.size();
            net = cell(cell_outputs[n-1], cell_outputs[n-2]);
            cell_outputs.push_back(net);
            // Update filter scaling.
            filter_scaling *= m_filter_scaling_rate;
        }
        return cell_outputs;
    }
    /** Reduction layers: bool vector */
    std::vector<bool> reduction_layers() const {
        std::vector<bool> r_layers(m_num_cells, false);
        for (size_t i = 1 ; i <= m_num_reduction_layers ; ++i) {
            float l_idx = (float(i) / float(m_num_reduction_layers + 1)) * m_num_cells;
            r_layers[int(l_idx)] = true;
        }
        return r_layers;
    }

protected:
    /** Stem multiplier. */
    float  m_stem_multiplier;
    /** Filter scaling. */
    float  m_filter_scaling_rate;
    /** Number of cells. */
    size_t  m_num_cells;
    /** Number of reduction layers. */
    size_t  m_num_reduction_layers;
    /** Number of convolutional filters. */
    size_t  m_num_conv_filters;
};



}

/* ============================================================================
 * NASNet mobile.
 * ========================================================================== */
namespace nasnet_mobile
{
class net : public nasnet::net
{
public:
    /** NASNet mobile definition. */
    net() : nasnet::net("nasnet_mobile", 1.0, 2.0, 12, 2, 44)
    {}

    // stem_multiplier=1.0,
    // dense_dropout_keep_prob=0.5,
    // num_cells=12,
    // filter_scaling_rate=2.0,
    // drop_path_keep_prob=1.0,
    // num_conv_filters=44,
    // use_aux_head=1,
    // num_reduction_layers=2,
    // data_format='NHWC',
    // skip_reduction_layer_input=0,
    // total_training_steps=250000,
};
}
/* ============================================================================
 * NASNet large.
 * ========================================================================== */
namespace nasnet_large
{
class net : public nasnet::net
{
public:
    /** NASNet mobile definition. */
    net() : nasnet::net("nasnet_large", 3.0, 2.0, 18, 2, 168)
    {}
    // stem_multiplier=3.0,
    // dense_dropout_keep_prob=0.5,
    // num_cells=18,
    // filter_scaling_rate=2.0,
    // num_conv_filters=168,
    // drop_path_keep_prob=drop_path_keep_prob,
    // use_aux_head=1,
    // num_reduction_layers=2,
    // data_format='NHWC',
    // skip_reduction_layer_input=1,
    // total_training_steps=250000,
};
}
#endif
