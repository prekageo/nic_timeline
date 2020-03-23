#!/usr/bin/env python3

import matplotlib
matplotlib.use('agg')
import matplotlib.pyplot as plt
import numpy as np
import sys

def median(l):
    return l[len(l)//2]

BOX_HEIGHT = 0.05

class Item:
    def __init__(self, title, x, y, z, alignment, rx):
        self.title = title
        self.x = x
        self.y = y
        self.z = z
        self.alignment = alignment
        self.rx = rx
        self.data = []

items = {}

def add_item(item):
    items[item.title] = item

add_item(Item('PCI write'                    ,1  ,.9 ,0   ,'left'  ,False))
add_item(Item('NIC fetches TX descriptor'    ,1  ,.83,0   ,'center',False))
add_item(Item('NIC fetches payload'          ,1  ,.75,-400,'center',False))
add_item(Item('NIC updates TX descriptor'    ,1  ,.67,-1e3,'left'  ,False))
add_item(Item('NIC updates TDH register'     ,.85,.75,-1e3,'left'  ,False))
add_item(Item('TX timestamp'                 ,1  ,.9 ,0   ,'center',False))

add_item(Item('NIC writes payload'           ,1  ,.83,-200,'right' ,True))
add_item(Item('NIC updates RDH register'     ,.85,.75,0   ,'center',True))
add_item(Item('NIC fetches DMA RX descriptor',1  ,.9 ,0   ,'center',True))
add_item(Item('RX timestamp'                 ,1  ,.9 ,0   ,'center',True))
add_item(Item('[TX/RX] round trip'           ,0  ,0  ,0   ,'center',False))

def plot(filename):
    f = open(filename, 'r')
    while True:
        id = f.readline().strip()
        if len(id) == 0:
            break
        l = list(map(float, f.readline().split()))
        l.sort()
        l = l[3:-3]
        if id in items:
            items[id].data = l
        else:
            print('ignore %s' % id)

    one_way_median = median(items['[TX/RX] round trip'].data) / 2

    delete = []
    for item in items.values():
        if len(item.data) == 0:
            delete.append(item.title)
    for d in delete:
        del items[d]

    for item in items.values():
        if item.rx:
            item.data = [one_way_median - x for x in item.data]
            item.data.reverse()

    print('== %s' % filename)
    print('%10s %10s %10s' % ('min', 'max', 'median'))
    for item in sorted(items.values(), key = lambda item: median(item.data)):
        print('%10.1f %10.1f %10.1f %s' % (item.data[0], item.data[-1], median(item.data), item.title))

    fig = plt.figure(figsize=(18, 6.5))
    ax = fig.add_subplot(111)
    ax.set_xlim(0,6000)
    ax.plot([0, max(items['RX interrupt'].data)], [1,1])
    wire_start = median(items['TX timestamp'].data)
    wire_stop = median(items['RX timestamp'].data)
    ax.plot([wire_start, wire_stop], [1.11,1.11])
    ax.text((wire_start + wire_stop)/2,1.13, 'on the wire = %.1f ns' % (wire_stop - wire_start), horizontalalignment='center', verticalalignment='center')
    ax.plot([0, one_way_median], [1.17,1.17])
    ax.text(one_way_median/2,1.17, 'one-way latency = %.1f ns' % one_way_median, horizontalalignment='center', verticalalignment='center', bbox = dict(boxstyle="round", fc="0.8"))
    ax.spines['top'].set_visible(False)
    ax.spines['right'].set_visible(False)
    ax.spines['left'].set_visible(False)
    for item in items.values():
        p = item.x
        v = item.data
        ax.boxplot(v, False, None, False, widths = BOX_HEIGHT, positions = (p,))
        ax.annotate('%s\n%.1f ns' % (item.title, median(v)), (median(v), p - BOX_HEIGHT/2),
            xytext = (median(v) + item.z,item.y),
            arrowprops = dict(arrowstyle = "->",connectionstyle = "angle,angleA=0,angleB=90"),
            bbox = dict(boxstyle="round", fc="0.8"),
            horizontalalignment=item.alignment,
        )
        ax.set_ylim(0.55,1.2)
        z0 = fig.transFigure.inverted().transform(ax.transData.transform([v[0],p + BOX_HEIGHT/2 + 0.01]))
        z1 = fig.transFigure.inverted().transform(ax.transData.transform([v[-1],p + .07 + BOX_HEIGHT/2 + 0.01]))
        frq, edges = np.histogram(v, bins=max(1, int((v[-1]-v[0])/20)))
        ax2 = fig.add_axes([z0[0],z0[1],z1[0]-z0[0],z1[1]-z0[1]])
        ax2.bar(edges[:-1], frq, width=np.diff(edges), ec="k", align="edge")
        ax2.set_xlim(v[0],v[-1])
        ax2.axis('off')
    ax.get_yaxis().set_visible(False)
    ax.set_xlabel('Time (ns)')
    ax.set_title(filename)
    fig.savefig(filename[:-4] + '_nic_timeline.png')

def main():
    for f in sys.argv[1:]:
        plot(f)

if __name__ == "__main__":
    main()
