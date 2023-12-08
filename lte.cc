#include "ns3/applications-module.h"
#include "ns3/config-store-module.h"
#include "ns3/core-module.h"
#include "ns3/flow-monitor-module.h"
#include "ns3/gnuplot.h"
#include "ns3/internet-module.h"
#include "ns3/lte-module.h"
#include "ns3/mobility-module.h"
#include "ns3/netanim-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"

#include <math.h>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("lte_scenario");

void
ThroughputMonitor(FlowMonitorHelper* fmhelper, Ptr<FlowMonitor> flowMon, Gnuplot2dDataset DataSet)
{
    double localThroughput = 0;

    std::map<FlowId, FlowMonitor::FlowStats> flowStats = flowMon->GetFlowStats();
    Ptr<Ipv4FlowClassifier> classing = DynamicCast<Ipv4FlowClassifier>(fmhelper->GetClassifier());

    for (std::map<FlowId, FlowMonitor::FlowStats>::const_iterator stats = flowStats.begin();
         stats != flowStats.end();
         ++stats)
    {
        Ipv4FlowClassifier::FiveTuple fiveTuple = classing->FindFlow(stats->first);
        
        if (fiveTuple.sourceAddress == Ipv4Address("1.0.0.2"))
        {
            std::cout << "Flow ID    : " << stats->first << " ; " << fiveTuple.sourceAddress
                      << " -----> " << fiveTuple.destinationAddress << std::endl;
            std::cout << "Tx Packets : " << stats->second.txPackets << std::endl;
            std::cout << "Rx Packets : " << stats->second.rxPackets << std::endl;
            std::cout << "Throughput : "
                      << stats->second.rxBytes * 8.0 /
                             (stats->second.timeLastRxPacket.GetSeconds() -
                              stats->second.timeFirstTxPacket.GetSeconds()) /
                             1024
                      << " Kbps" << std::endl;

            localThroughput = (stats->second.rxBytes * 8.0 /
                          (stats->second.timeLastRxPacket.GetSeconds() -
                           stats->second.timeFirstTxPacket.GetSeconds()) /
                          1024);
                          
            DataSet.Add((double)Simulator::Now().GetSeconds(), (double)localThroughput);

            std::cout
                << "---------------------------------------------------------------------------"
                << std::endl;
        }
    }

    Simulator::Schedule(Seconds(0.2), &ThroughputMonitor, fmhelper, flowMon, DataSet);
    flowMon->SerializeToXmlFile("ThroughputMonitor.xml", true, true);
}

int
main(int argc, char* argv[])
{
    int ueNumber = 30;
    Time simulationTime = Seconds(60.0);

    Config::SetDefault("ns3::UdpClient::Interval", TimeValue(MilliSeconds(20)));
    Config::SetDefault("ns3::UdpClient::MaxPackets", UintegerValue(1000000));
    Config::SetDefault("ns3::LteEnbRrc::SrsPeriodicity", UintegerValue(320));
    Config::SetDefault("ns3::LteHelper::UseIdealRrc", BooleanValue(false));

    CommandLine cmd;
    cmd.AddValue("numberOfUes", "Number of UEs", ueNumber);
    cmd.Parse(argc, argv);

    Ptr<LteHelper> lteHelper = CreateObject<LteHelper>();
    Ptr<PointToPointEpcHelper> epcHelper = CreateObject<PointToPointEpcHelper>();
    lteHelper->SetEpcHelper(epcHelper);
    lteHelper->SetSchedulerType("ns3::RrFfMacScheduler");
    lteHelper->SetHandoverAlgorithmType("ns3::NoOpHandoverSDSAlgorithm");

    Ptr<Node> pgw = epcHelper->GetPgwNode();

    NodeContainer remoteHostContainer;
    remoteHostContainer.Create(1);
    Ptr<Node> remoteHost = remoteHostContainer.Get(0);

    InternetStackHelper internet;
    internet.Install(remoteHostContainer);

    PointToPointHelper p2ph;
    p2ph.SetDeviceAttribute("DataRate", DataRateValue(DataRate("100Gb/s")));
    p2ph.SetDeviceAttribute("Mtu", UintegerValue(1500));
    p2ph.SetChannelAttribute("Delay", TimeValue(Seconds(0.010)));

    NetDeviceContainer internetDevices = p2ph.Install(pgw, remoteHost);

    Ipv4AddressHelper ipv4h;
    ipv4h.SetBase("1.0.0.0", "255.0.0.0");

    Ipv4InterfaceContainer internetIpIfaces = ipv4h.Assign(internetDevices);
    Ipv4Address remoteHostAddr = internetIpIfaces.GetAddress(1);

    Ipv4StaticRoutingHelper ipv4RoutingHelper;
    Ptr<Ipv4StaticRouting> remoteHostStaticRouting =
        ipv4RoutingHelper.GetStaticRouting(remoteHost->GetObject<Ipv4>());
    remoteHostStaticRouting->AddNetworkRouteTo(Ipv4Address("7.0.0.0"), Ipv4Mask("255.0.0.0"), 1);

    NodeContainer enbNodes;
    enbNodes.Create(1);

    NodeContainer ueNodes;
    ueNodes.Create(ueNumber);

    MobilityHelper mobilityHelper;
    mobilityHelper.SetPositionAllocator("ns3::GridPositionAllocator",
                                  "MinX",
                                  DoubleValue(10.0),
                                  "MinY",
                                  DoubleValue(10.0),
                                  "DeltaX",
                                  DoubleValue(12.0),
                                  "DeltaY",
                                  DoubleValue(10.0),
                                  "GridWidth",
                                  UintegerValue(5),
                                  "LayoutType",
                                  StringValue("RowFirst"));
    mobilityHelper.SetMobilityModel("ns3::RandomWalk2dMobilityModel",
                              "Bounds",
                              RectangleValue(Rectangle(-500, 500, -250, 500)));
    mobilityHelper.Install(ueNodes);
    
    mobilityHelper.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    mobilityHelper.Install(enbNodes);

    NetDeviceContainer enbDevices = lteHelper->InstallEnbDevice(enbNodes);
    NetDeviceContainer ueDevices = lteHelper->InstallUeDevice(ueNodes);

    internet.Install(ueNodes);

    Ipv4InterfaceContainer ueIpIfaces;
    ueIpIfaces = epcHelper->AssignUeIpv4Address(NetDeviceContainer(ueDevices));

    lteHelper->AttachToClosestEnb(ueDevices, enbDevices);

    NS_LOG_LOGIC("Setting up applications");

    int dlPort = 10000;
    int ulPort = 20000;

    // randomize a bit start times to avoid simulation artifacts
    // (e.g., buffer overflows due to packet transmissions happening exactly at the same time)
    Ptr<UniformRandomVariable> startTimeSeconds = CreateObject<UniformRandomVariable>();
    startTimeSeconds->SetAttribute("Min", DoubleValue(0));
    startTimeSeconds->SetAttribute("Max", DoubleValue(0.010));

    for (int u = 0; u < ueNumber; ++u)
    {
        Ptr<Node> ue = ueNodes.Get(u);

        Ptr<Ipv4StaticRouting> ueStaticRouting =
            ipv4RoutingHelper.GetStaticRouting(ue->GetObject<Ipv4>());
        ueStaticRouting->SetDefaultRoute(epcHelper->GetUeDefaultGatewayAddress(), 1);

        ++dlPort;
        ++ulPort;

        ApplicationContainer clientApps;
        ApplicationContainer serverApps;

        NS_LOG_LOGIC("Installing UDP DL app for UE " << u);

        UdpClientHelper dlClientHelper(ueIpIfaces.GetAddress(u), dlPort);
        clientApps.Add(dlClientHelper.Install(remoteHost));
        PacketSinkHelper dlPacketSinkHelper("ns3::UdpSocketFactory",
                                            InetSocketAddress(Ipv4Address::GetAny(), dlPort));
        serverApps.Add(dlPacketSinkHelper.Install(ue));

        NS_LOG_LOGIC("Installing UDP UL app for UE " << u);
        
        UdpClientHelper ulClientHelper(remoteHostAddr, ulPort);
        clientApps.Add(ulClientHelper.Install(ue));
        PacketSinkHelper ulPacketSinkHelper("ns3::UdpSocketFactory",
                                            InetSocketAddress(Ipv4Address::GetAny(), ulPort));
        serverApps.Add(ulPacketSinkHelper.Install(remoteHost));

        Ptr<EpcTft> tft = Create<EpcTft>();
        EpcTft::PacketFilter dlpf;
        dlpf.localPortStart = dlPort;
        dlpf.localPortEnd = dlPort;
        tft->Add(dlpf);
        EpcTft::PacketFilter ulpf;
        ulpf.remotePortStart = ulPort;
        ulpf.remotePortEnd = ulPort;
        tft->Add(ulpf);

        EpsBearer bearer(EpsBearer::GBR_CONV_VOICE);
        lteHelper->ActivateDedicatedEpsBearer(ueDevices.Get(u), bearer, tft);

        Time startTime = Seconds(startTimeSeconds->GetValue());
        serverApps.Start(startTime);
        clientApps.Start(startTime);
    }

    lteHelper->AddX2Interface(enbNodes);

    lteHelper->EnablePhyTraces();
    lteHelper->EnableMacTraces();
    lteHelper->EnableRlcTraces();
    lteHelper->EnablePdcpTraces();

    lteHelper->SetPathlossModelType(FriisPropagationLossModel::GetTypeId());

    Ptr<RadioBearerStatsCalculator> rlcStats = lteHelper->GetRlcStats();
    rlcStats->SetAttribute("EpochDuration", TimeValue(Seconds(0.02)));
    Ptr<RadioBearerStatsCalculator> pdcpStats = lteHelper->GetPdcpStats();
    pdcpStats->SetAttribute("EpochDuration", TimeValue(Seconds(0.02)));

    Simulator::Stop(simulationTime);

    AnimationInterface anim("lte2.xml");
    anim.SetMaxPktsPerTraceFile(100000000000);
    anim.SetMobilityPollInterval(Seconds(1));

    std::string fileNameWithNoExtension = "FlowVSThroughput_";
    std::string graphicsFileName = fileNameWithNoExtension + ".png";
    std::string plotFileName = fileNameWithNoExtension + ".plt";
    std::string plotTitle = "Flow vs Throughput";
    std::string dataTitle = "Throughput";

    Gnuplot gnuplot(graphicsFileName);
    gnuplot.SetTitle(plotTitle);
    gnuplot.SetTerminal("png");
    gnuplot.SetLegend("Flow", "Throughput");

    Gnuplot2dDataset dataset;
    dataset.SetTitle(dataTitle);
    dataset.SetStyle(Gnuplot2dDataset::LINES_POINTS);

    FlowMonitorHelper flowMonitor;
    Ptr<FlowMonitor> allMonitor = flowMonitor.InstallAll();
    allMonitor->CheckForLostPackets();

    ThroughputMonitor(&flowMonitor, allMonitor, dataset);

    Simulator::Run();

    gnuplot.AddDataset(dataset);

    std::ofstream plotFile(plotFileName.c_str());

    gnuplot.GenerateOutput(plotFile);
    plotFile.close();

    Simulator::Destroy();
    return 0;
}