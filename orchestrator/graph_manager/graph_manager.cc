#include "graph_manager.h"

pthread_mutex_t GraphManager::graph_manager_mutex;
uint32_t GraphManager::nextControllerPort = FIRTS_OF_CONTROLLER_PORT;

void GraphManager::mutexInit()
{
	pthread_mutex_init(&graph_manager_mutex, NULL);
}

GraphManager::GraphManager(int core_mask, bool wireless, char *wirelessName) :
	xDPDManager(string(XDPD_PORT))
{
	//TODO: we may have two implementations: one with the LSI-0, the other that uses the queues of the NIC

	//Create the openflow controller for the LSI-0

	pthread_mutex_lock(&graph_manager_mutex);
	uint32_t controllerPort = nextControllerPort;
	nextControllerPort++;
	pthread_mutex_unlock(&graph_manager_mutex);
	
	ostringstream strControllerPort;
	strControllerPort << controllerPort;

	//Create the LSI-0 with all the phy ports managed by xDPD
	map<string,string> phyPorts;
	
	logger(ORCH_INFO, MODULE_NAME, __FILE__, __LINE__, "Discovering the available physical interfaces...");
	try
	{
		phyPorts = xDPDManager.discoverPhyPorts();
	} catch (...)
	{
		throw GraphManagerException();
	}
	
	logger(ORCH_INFO, MODULE_NAME, __FILE__, __LINE__, "\t%d available physical interfaces:",phyPorts.size());
	for(map<string,string>::iterator p = phyPorts.begin(); p != phyPorts.end(); p++)
		logger(ORCH_INFO, MODULE_NAME, __FILE__, __LINE__, "\t\t%s (%s)",p->first.c_str(),p->second.c_str());

	logger(ORCH_INFO, MODULE_NAME, __FILE__, __LINE__, "Creating the LSI-0...");

	//The three following strunctures are empty. No NF and no virtual link is attached.	
	map<string, list<unsigned int> > dummy_network_functions;
	vector<VLink> dummy_virtual_links;
	map<string,nf_t>  nf_types;
	
	//XXX: if(wireless == true), the node has a wireless interface. This kind of interface is not supported 
	//by the DPDK (and hence by xDPd), then it is attached to xDPd with a trick. In practice, xDPd creates a 
	//KNI port, and then we attach this KNI to a bride, which is in turn connected to the wireless interface
	
	LSI *lsi = new LSI(string(OF_CONTROLLER_ADDRESS), strControllerPort.str(), phyPorts, dummy_network_functions,dummy_virtual_links,nf_types, (wireless)? string(wirelessName) : "" );
	
	try
	{
		xDPDManager.createLsi(*lsi);
	} catch (XDPDManagerException e)
	{
		logger(ORCH_ERROR, MODULE_NAME, __FILE__, __LINE__, "%s",e.what());
		throw GraphManagerException();
	}
	
	dpid0 = lsi->getDpid();
	map<string,unsigned int> lsi_ports = lsi->getEthPorts();
			
	logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "LSI ID: %d",dpid0);
	logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "Ethernet ports:",lsi_ports.size());
	for(map<string,unsigned int>::iterator p = lsi_ports.begin(); p != lsi_ports.end(); p++)
		logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "\t%s -> %d",(p->first).c_str(),p->second);
		
	if(wireless)
	{
		logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "Wireless port: %s -> %d",wirelessName, lsi->getWirelessPort().second);
	
		
		if(!attachWirelessPort(lsi))
		{
			logger(ORCH_ERROR, MODULE_NAME, __FILE__, __LINE__, "An error occurred during the initialization of the wireless interface \"%s\"",wirelessName);
			throw GraphManagerException();
		}

	}

	graphInfoLSI0.setLSI(lsi);

	//Create the openflow controller for the lsi-0
	logger(ORCH_INFO, MODULE_NAME, __FILE__, __LINE__, "Creating the openflow controller for LSI-0...");

	lowlevel::Graph graph;

	rofl::openflow::cofhello_elem_versionbitmap versionbitmap;
	versionbitmap.add_ofp_version(rofl::openflow12::OFP_VERSION);
	
	Controller *controller = new Controller(versionbitmap,graph,strControllerPort.str());
	controller->start();

	graphInfoLSI0.setController(controller);
	
	logger(ORCH_INFO, MODULE_NAME, __FILE__, __LINE__, "LSI-0 and its controller are created");

	NFsManager::setCoreMask(core_mask);
}

GraphManager::~GraphManager()
{	
	//Deleting tenants LSIs
	for(map<string,GraphInfo>::iterator lsi = tenantLSIs.begin(); lsi != tenantLSIs.end();)
	{	
		logger(ORCH_INFO, MODULE_NAME, __FILE__, __LINE__, "Deleting the graph'%s'...",lsi->first.c_str());
		map<string,GraphInfo>::iterator tmp = lsi;
		lsi++;
		try
		{
			deleteGraph(tmp->first, true);
		}catch(...)
		{
			assert(0);
			/*nothing to do, since the node orchestrator is terminating*/
		}
	}
	
	//Deleting LSI-0
	logger(ORCH_INFO, MODULE_NAME, __FILE__, __LINE__, "Deleting the graph for the LSI-0...");
	LSI *lsi0 = graphInfoLSI0.getLSI();
	
	detachWirelessPort(lsi0);
	try
	{
		xDPDManager.destroyLsi(*lsi0);
	} catch (XDPDManagerException e)
	{
		logger(ORCH_WARNING, MODULE_NAME, __FILE__, __LINE__, "%s",e.what());
		//we don't throw any exception here, since the graph manager is terminating
	}
	
	Controller *controller = graphInfoLSI0.getController();
	delete(controller);
	controller = NULL;
}

bool GraphManager::graphExists(string graphID)
{
	if(tenantLSIs.count(graphID) == 0)
		return false;
		
	return true;
}

bool GraphManager::graphContainsNF(string graphID,string nf)
{
	if(!graphExists(graphID))
		return false;

	GraphInfo graphInfo = (tenantLSIs.find(graphID))->second;
	highlevel::Graph *graph = graphInfo.getGraph();		
	
	return graph->stillExistNF(nf);
}

bool GraphManager::flowExists(string graphID, string flowID)
{
	assert(tenantLSIs.count(graphID) != 0);
	
	GraphInfo graphInfo = (tenantLSIs.find(graphID))->second;
	highlevel::Graph *graph = graphInfo.getGraph();
	
	if(!graph->ruleExists(flowID))
		return false;
	
	return true;
}

Object GraphManager::toJSON(string graphID)
{
	if(tenantLSIs.count(graphID) == 0)
	{
		logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "The graph \"%s\" does not exist",graphID.c_str());
		assert(0);
		throw GraphManagerException();
	}
	highlevel::Graph *graph = (tenantLSIs.find(graphID))->second.getGraph();
	assert(graph != NULL);
	
	Object flow_graph;
	
	try
	{
		flow_graph[FLOW_GRAPH] = graph->toJSON();	
	}catch(...)
	{
		assert(0);
		throw GraphManagerException();
	}
	
	return flow_graph;
}

Object GraphManager::toJSONPhysicalInterfaces()
{
	Object interfaces;
	
	LSI *lsi0 = graphInfoLSI0.getLSI();
	
	map<string,string> types = lsi0->getPortsType();
	
	Array interfaces_array;
	for(map<string,string>::iterator t = types.begin(); t != types.end(); t++)
	{
		Object iface;
		iface["name"] = t->first;
		iface["type"] = t->second;
		interfaces_array.push_back(iface);	
	}
	
	if(lsi0->hasWireless())
	{
		Object iface;
		iface["name"] = lsi0->getWirelessPortName();
		iface["type"] = "edge";
		interfaces_array.push_back(iface);
	}
	
	interfaces["interfaces"] = interfaces_array;
	
	return interfaces;
}

bool GraphManager::deleteGraph(string graphID, bool shutdown)
{
	if(tenantLSIs.count(graphID) == 0)
	{
		logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "The graph \"%s\" does not exist",graphID.c_str());
		return false;
	}
	
	/**
	*	@outline:
	*
	*		0) check if the graph can be remode
	*		1) remove the rules from the LSI0
	*		2) stop the NFs
	*		3) delete the LSI, the virtual links and the 
	*			ports related to NFs
	*		4) delete the endpoints defined by the graph
	*/
	
	logger(ORCH_INFO, MODULE_NAME, __FILE__, __LINE__, "Deleting graph '%s'...",graphID.c_str());

	LSI *tenantLSI = (tenantLSIs.find(graphID))->second.getLSI();
	highlevel::Graph *highLevelGraph = (tenantLSIs.find(graphID))->second.getGraph();

	/**
	*		0) check if the graph can be removed
	*/
	if(!shutdown)
	{
		set<string> endpoints = highLevelGraph->getEndPoints();
		for(set<string>::iterator ep = endpoints.begin(); ep != endpoints.end(); ep++)
		{
			if(highLevelGraph->isDefinedHere(*ep))
			{
				if(availableEndPoints.find(*ep)->second !=0)
				{
					logger(ORCH_INFO, MODULE_NAME, __FILE__, __LINE__, "The graph cannot be deleted. It defines the endpoint \"%s\" that is used %d times in other graphs; first remove the rules in those graphs.",ep->c_str(),availableEndPoints.find(*ep)->second);
					return false;
				}
			}
		}
	}
	
	/**
	*		1) remove the rules from the LSI-0
	*/
	logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "1) Remove the rules from the LSI-0");
	
	lowlevel::Graph graphLSI0 = GraphTranslator::lowerGraphToLSI0(highLevelGraph,tenantLSI,graphInfoLSI0.getLSI(), endPointsDefinedInMatches, endPointsDefinedInActions, availableEndPoints, false);	
	
	//Remove rules from the LSI-0
	graphInfoLSI0.getController()->removeRules(graphLSI0.getRules());
	
	/**
	*		2) stop the NFs
	*/
	NFsManager *nfsManager = (tenantLSIs.find(graphID))->second.getNFsManager();
#ifdef RUN_NFS
	logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "2) Stop the NFs");
	nfsManager->stopAll();
#else
	logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "2) Flag RUN_NFS disabled. No NF to be stopped");
#endif
	
	/**
	*		3) delete the LSI, the virtual links and the 
	*			ports related to NFs
	*/
	logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "3) Delete the LSI, the vlinks, and the ports used by NFs");
	
	try
	{
		xDPDManager.destroyLsi(*tenantLSI);
	} catch (XDPDManagerException e)
	{
		logger(ORCH_WARNING, MODULE_NAME, __FILE__, __LINE__, "%s",e.what());
		throw GraphManagerException();
	}

	detachWirelessPort(tenantLSI);
	
	/**
	*		4) delete the endpoints defined by the graph
	*/
	if(!shutdown)
	{
		set<string> endpoints = highLevelGraph->getEndPoints();
		for(set<string>::iterator ep = endpoints.begin(); ep != endpoints.end();)
		{
			if(highLevelGraph->isDefinedHere(*ep))
			{
				assert(availableEndPoints.find(*ep)->second ==0);
				assert(endPointsDefinedInMatches.count(*ep) != 0 || endPointsDefinedInActions.count(*ep) != 0);
				
				set<string>::iterator tmp = ep;
				ep++;
				
				availableEndPoints.erase(*tmp);
				if(endPointsDefinedInActions.count(*tmp) != 0)
					endPointsDefinedInActions.erase(*tmp);
				if(endPointsDefinedInMatches.count(*tmp) != 0)
					endPointsDefinedInMatches.erase(*tmp);
				
				logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "The endpoint \"%s\" is no longer available",tmp->c_str());
			}
			else
				ep++;
		}
	}
	
	tenantLSIs.erase(tenantLSIs.find(highLevelGraph->getID()));

	delete(highLevelGraph);
	delete(tenantLSI);
	delete(nfsManager);

	highLevelGraph = NULL;
	tenantLSI = NULL;
	nfsManager = NULL;
	
	return true;
}

bool GraphManager::deleteFlow(string graphID, string flowID)
{
	if(tenantLSIs.count(graphID) == 0)
	{
		logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "The graph \"%s\" does not exist",graphID.c_str());
		assert(0);
		return false;
	}
	
	GraphInfo graphInfo = (tenantLSIs.find(graphID))->second;
	highlevel::Graph *graph = graphInfo.getGraph();
	
	if(!graph->ruleExists(flowID))
	{
		logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "The flow \"%s\" does not exist in graph \"%s\"",flowID.c_str(),graphID.c_str());
		assert(0);
		return false;
	}

	//if the graph has only this flow, remove the entre graph
	if(graph->getNumberOfRules() == 1)
	{
		logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "The graph \"%s\" has only one flow. Then the entire graph will be removed",graphID.c_str());
		return deleteGraph(graphID);
	}
	
	//Unfortunately this is not the only flow of the graph
	
	/**
	*	The flow can be removed only if does not define an endpoint used by some other graph
	*/
	if(!canDeleteFlow(graph,flowID))
		return false;
		
	string endpointInvolved = graph->getEndpointInvolved(flowID);
	bool definedHere = false;
	if(endpointInvolved != "")
		definedHere = graph->isDefinedHere(endpointInvolved);
	
	logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "Removing the flow from the LSI-0 graph");
	Controller *lsi0Controller = graphInfoLSI0.getController();
	stringstream lsi0FlowID;
	lsi0FlowID << graph->getID() << "_" << flowID;
	lsi0Controller->removeRuleFromID(lsi0FlowID.str());
	
	logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "Removing the flow from the tenant-LSI graph");
	Controller *tenantController = graphInfo.getController();
	tenantController->removeRuleFromID(flowID);
	
	logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "Removing the flow from the high level graph");
	RuleRemovedInfo rri = graph->removeRuleFromID(flowID);
	
	NFsManager *nfs_manager = graphInfo.getNFsManager();
	LSI *lsi = graphInfo.getLSI();
	
	removeUselessPorts_NFs_Endpoints_VirtualLinks(rri,nfs_manager,graph,lsi);
	
	if(endpointInvolved != "")
	{
		if(definedHere)
		{
			availableEndPoints.erase(endpointInvolved);
			if(endPointsDefinedInActions.count(endpointInvolved) != 0)
				endPointsDefinedInActions.erase(endpointInvolved);
			if(endPointsDefinedInMatches.count(endpointInvolved) != 0)
				endPointsDefinedInMatches.erase(endpointInvolved);
		}
		else
		{
			availableEndPoints[endpointInvolved]--;
			logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "endpoint \"%s\" still used %d times",endpointInvolved.c_str(), availableEndPoints[endpointInvolved]);
		}
	}
	
	return true;
}

bool GraphManager::checkGraphValidity(highlevel::Graph *graph, NFsManager *nfsManager)
{
	set<string> phyPorts = graph->getPorts();
	set<string> endPoints = graph->getEndPoints();

	string graphID = graph->getID();
	
	logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "The command requires %d new physical ports",phyPorts.size());
	
	LSI *lsi0 = graphInfoLSI0.getLSI();
	map<string,unsigned int> ethPorts = lsi0->getEthPorts();
	pair<string,unsigned int> wirelessPort;
	
	if(lsi0->hasWireless())
		wirelessPort = lsi0->getWirelessPort();
	
	for(set<string>::iterator p = phyPorts.begin(); p != phyPorts.end(); p++)
	{
		if((ethPorts.count(*p)) == 0 && (wirelessPort.first != *p))
		{
			logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "Physical port \"%s\" does not exist",(*p).c_str());
			return false;		
		}
	}
	
	logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "The command requires %d graph endpoints (i.e., logical ports to be used to connect two graphs together)",endPoints.size());
	
	for(set<string>::iterator graphEP = endPoints.begin(); graphEP != endPoints.end(); graphEP++)
	{
		if(!graph->isDefinedHere(*graphEP))
		{
			//since this endpoint is defined into another graph, that endpoint must alredy exist
			if(availableEndPoints.count(*graphEP) == 0)
			{
				logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "Endpoint \"%s\" is not defined by the current graph, and it does not exist yet",graphEP->c_str());
				return false;
			}
			
			if(graph->endpointIsUsedInMatch(*graphEP))
			{
				//Another graph must have been defined it in an action
				if(endPointsDefinedInActions.count(*graphEP) == 0)
				{
					logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "Endpoint \"%s\" is used in a match of the current graph, but it was not defined in an action of another graph",graphEP->c_str());
					return false;
				}
			}
			if(graph->endpointIsUsedInAction(*graphEP))
			{
				//Another graph must have been defined it in a match
				if(endPointsDefinedInMatches.count(*graphEP) == 0)
				{
					logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "Endpoint \"%s\" is used in an action of the current graph, but it was not defined in a match of another graph",graphEP->c_str());
					return false;
				}
			}
		}
	}
	
	map<string,list<unsigned int> > network_functions = graph->getNetworkFunctions();

	logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "The command requires to retrieve %d new NFs",network_functions.size());

	list<string> requiredNFs;
	for(map<string,list<unsigned int> >::iterator nf = network_functions.begin(); nf != network_functions.end(); nf++)
	{
		nf_manager_ret_t retVal = nfsManager->retrieveDescription(nf->first);
	
		if(retVal == NFManager_NO_NF)
		{
			logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "NF \"%s\" cannot be retrieved",nf->first.c_str());
			return false;
		}
		else if(retVal == NFManager_SERVER_ERROR)
		{
			throw GraphManagerException();
		}
	}

	return true;
}

void *startNF(void *arguments)
{
    to_thread_t *args = (to_thread_t *)arguments;
    assert(args->nfsManager != NULL);
    
    if(!args->nfsManager->startNF(args->nf_name, args->number_of_ports, args->ipv4PortsRequirements, args->ethPortsRequirements))
    	return (void*) 0;
    else
    	return (void*) 1;
}


bool GraphManager::newGraph(highlevel::Graph *graph)
{	
	logger(ORCH_INFO, MODULE_NAME, __FILE__, __LINE__, "Creating a new graph '%s'...",graph->getID().c_str());
	
	assert(tenantLSIs.count(graph->getID()) == 0);
	
	/**
	*	@outline:
	*
	*		0) check the validity of the graph
	*		1) create the Openflow controller for the tenant LSI
	*		2) select an implementation for each NF of the graph
	*		3) create the LSI, with the proper ports
	*		4) start the NFs
	*		5) download the rules in LSI-0 and tenant-LSI
	*/
	
	/**
	*	0) Check the validity of the graph
	*/
	logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "0) Check the validity of the graph");
	
	NFsManager *nfsManager = new NFsManager();
	
	if(!checkGraphValidity(graph,nfsManager))
	{
		//This is an error in the request
		delete(nfsManager);
		nfsManager = NULL;
		return false;
	}

	/**
	*	1) Create the Openflow controller for the tenant LSI
	*/
	logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "1) Create the Openflow controller for the tenant LSI");
	
	pthread_mutex_lock(&graph_manager_mutex);
	uint32_t controllerPort = nextControllerPort;
	nextControllerPort++;
	pthread_mutex_unlock(&graph_manager_mutex);

	ostringstream strControllerPort;
	strControllerPort << controllerPort;

	rofl::openflow::cofhello_elem_versionbitmap versionbitmap;
	versionbitmap.add_ofp_version(rofl::openflow12::OFP_VERSION);

	lowlevel::Graph graphTmp ;
	Controller *controller = new Controller(versionbitmap,graphTmp,strControllerPort.str());
	controller->start();
	
	/**
	*	2) Select an implementation for each network function of the graph
	*/
	logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "2) Select an implementation for each NF of the graph");	
	if(!nfsManager->selectImplementation())
	{
		//This is an internal error
		delete(nfsManager);
		delete(controller);
		nfsManager = NULL;
		controller = NULL;
		throw GraphManagerException();
	}	
		
	/**
	*	3) Create the LSI
	*/
	logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "3) Create the LSI");
	
	set<string> phyPorts = graph->getPorts();
	map<string, list<unsigned int> > network_functions = graph->getNetworkFunctions();
	
	vector<set<string> > vlVector = identifyVirtualLinksRequired(graph);
	set<string> vlNFs = vlVector[0];
	set<string> vlPhyPorts = vlVector[1];
	set<string> vlEndPoints = vlVector[2];
	set<string> NFsFromEndPoint = vlVector[3];
	
	/**
	*	A virtual link can be used in two direction, hence it can be shared between a NF port and a physical port.
	*	In principle a virtual link could aslo be shared between a NF port and an endpoint but, for simplicity, we
	*	use separated virtual links in case of endpoint.
	*/
	unsigned int numberOfVLrequiredBeforeEndPoints = (vlNFs.size() > vlPhyPorts.size())? vlNFs.size() : vlPhyPorts.size();
	unsigned int numberOfVLrequired = numberOfVLrequiredBeforeEndPoints + vlEndPoints.size();
	
	logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "%d virtual links are required to connect the new LSI with LSI-0",numberOfVLrequired);
	
	vector<VLink> virtual_links;
	for(unsigned int i = 0; i < numberOfVLrequired; i++)
		virtual_links.push_back(VLink(dpid0));
		
	//The tenant-LSI is not connected to physical ports, but just the LSI-0
	//through virtual links, and to network functions through virtual ports
	map<string,string> dummyPhyPorts;
	
	map<string,nf_t>  nf_types;
	for(map<string, list<unsigned int> >::iterator nf = network_functions.begin(); nf != network_functions.end(); nf++)
		nf_types[nf->first] = nfsManager->getNFType(nf->first);
	
	//Prepare the structure representing the new tenant-LSI
	LSI *lsi = new LSI(string(OF_CONTROLLER_ADDRESS), strControllerPort.str(), dummyPhyPorts, network_functions,virtual_links,nf_types);
	
	try
	{
		xDPDManager.createLsi(*lsi);
	} catch (XDPDManagerException e)
	{
		logger(ORCH_ERROR, MODULE_NAME, __FILE__, __LINE__, "%s",e.what());
		xDPDManager.destroyLsi(*lsi);
		delete(graph);
		delete(lsi);
		delete(nfsManager);
		delete(controller);
		graph = NULL;
		lsi = NULL;
		nfsManager = NULL;
		controller = NULL;
		throw GraphManagerException();
	}
	
	uint64_t dpid = lsi->getDpid();
	
	map<string,unsigned int> lsi_ports = lsi->getEthPorts();
	set<string> nfs = lsi->getNetworkFunctionsName();
	vector<VLink> vls = lsi->getVirtualLinks();
		
	logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "LSI ID: %d",dpid);
	
	logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "Ports (%d):",lsi_ports.size());
	for(map<string,unsigned int>::iterator p = lsi_ports.begin(); p != lsi_ports.end(); p++)
		logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "\t%s -> %d",(p->first).c_str(),p->second);
	
	logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "Network functions (%d):",nfs.size());
	for(set<string>::iterator it = nfs.begin(); it != nfs.end(); it++)
	{
		logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "\tNF %s:",it->c_str());
		map<string,unsigned int> nfs_ports = lsi->getNetworkFunctionsPorts(*it);
		logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "\t\t\tPorts (%d):",nfs_ports.size());
		for(map<string,unsigned int>::iterator n = nfs_ports.begin(); n != nfs_ports.end(); n++)
			logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "\t\t\t%s -> %d",(n->first).c_str(),n->second);
	}
	
	logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "Virtual links:",vls.size());
	for(vector<VLink>::iterator v = vls.begin(); v != vls.end(); v++)
		logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "\t(ID: %x) %x:%d -> %x:%d",v->getID(),dpid,v->getLocalID(),v->getRemoteDpid(),v->getRemoteID());

	//associate the vlinks to the NFs ports
	logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "NF port is virtual link ID:");
	map<string, uint64_t> nfs_vlinks;
	vector<VLink>::iterator vl1 = vls.begin();
	for(set<string>::iterator nf = vlNFs.begin(); nf != vlNFs.end(); nf++, vl1++)
	{
		nfs_vlinks[*nf] = vl1->getID();
		logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "\t%s -> %x",(*nf).c_str(),vl1->getID());
		
		if(NFsFromEndPoint.count(*nf) != 0)
		{
			//since this rule has a graph endpoint in the match that is defined in this
			//graph, we save the port of the vlink of the NF in LSI-0, so that other graphs can use this endpoint
			string ep = findEndPointTowardsNF(graph,*nf);
			logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "The endpoint \"%s\" is defined in a match of the current Graph. Other graphs can use it expressing an action on the port %d of the LSI-0",ep.c_str(),vl1->getRemoteID());
			endPointsDefinedInMatches[ep] = vl1->getRemoteID();
			
			//This endpoint is currently not used in any other graph, since it is defined in the current graph
			availableEndPoints[ep] = 0; 
		}
	}
	lsi->setNFsVLinks(nfs_vlinks);
	
	//associate the vlinks to the physical ports
	logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "Physical port is virtual link ID:");
	map<string, uint64_t> ports_vlinks;
	vector<VLink>::iterator vl2 = vls.begin();
	for(set<string>::iterator p = vlPhyPorts.begin(); p != vlPhyPorts.end(); p++, vl2++)
	{
		ports_vlinks[*p] = vl2->getID();
		logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "\t%s -> %x",(*p).c_str(),vl2->getID());
	}
	lsi->setPortsVLinks(ports_vlinks);
	
	//associate the vlinks to the endpoints
	logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "Endpoint is virtual link ID:");
	map<string, uint64_t> endpoints_vlinks;
	vector<VLink>::iterator vl3 = vls.begin();
	
	unsigned int aux = 0;
	while(aux < numberOfVLrequiredBeforeEndPoints)
	{
		//The first vlinks are only used for NFs and physical ports
		//TODO: this could be optimized, although it is not easy (and usefull)
		aux++;
		vl3++;
	}
	
	for(set<string>::iterator ep = vlEndPoints.begin(); ep != vlEndPoints.end(); ep++, vl3++)
	{			
		endpoints_vlinks[*ep] = vl3->getID();
		logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "\t%s -> %x",(*ep).c_str(),vl3->getID());
		if(graph->isDefinedHere(*ep))
		{
			//since this endpoint is in an action (hence it requires a virtual link), and it is defined in this
			//graph, we save the port of the vlink in LSI-0, so that other graphs can use this endpoint
					
			logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "The endpoint \"%s\" is defined in an action of the current Graph. Other graph can use it expressing a match on the port %d of the LSI-0",(*ep).c_str(),vl3->getRemoteID());
			endPointsDefinedInActions[*ep] = vl3->getRemoteID();
			
			//This endpoint is currently not used in any other graph, since it is defined in the current graph
			availableEndPoints[*ep] = 0; 
		}
	}
	lsi->setEndPointsVLinks(endpoints_vlinks);

	/**
	*	4) Start the network functions
	*/
#ifdef RUN_NFS
	logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "4) start the network functions");
	
	nfsManager->setLsiID(dpid);
		
	pthread_t some_thread[network_functions.size()];
	to_thread_t thr[network_functions.size()];
	int i = 0;
	for(map<string, list<unsigned int> >::iterator nf = network_functions.begin(); nf != network_functions.end(); nf++)
	{
		
		thr[i].nf_name = nf->first;
		thr[i].number_of_ports = nf->second.size();
		thr[i].ipv4PortsRequirements = graph->getNetworkFunctionIPv4PortsRequirements(nf->first);
		thr[i].ethPortsRequirements = graph->getNetworkFunctionEthernetPortsRequirements(nf->first);
		thr[i].nfsManager = nfsManager;
		
		if (pthread_create(&some_thread[i], NULL, &startNF, (void *)&thr[i]) != 0)
		{
			assert(0);
			logger(ORCH_ERROR, MODULE_NAME, __FILE__, __LINE__, "An error occurred while creating a new thread");
			throw GraphManagerException();	
		}
		i++;
	}
	
	bool ok = true;
	for(int j = 0; j < i;j++)
	{
		void *returnValue;
		pthread_join(some_thread[j], &returnValue);
		int *c = (int*)returnValue;
		
		if(c == 0)
			ok = false;
	}
	
	if(!ok)
	{
		for(map<string, list<unsigned int> >::iterator nf = network_functions.begin(); nf != network_functions.end(); nf++)
			nfsManager->stopNF(nf->first);

		xDPDManager.destroyLsi(*lsi);
	
		delete(graph);
		delete(lsi);
		delete(nfsManager);
		delete(controller);
		
		graph = NULL;;
		lsi = NULL;
		nfsManager = NULL;
		controller = NULL;
		
		throw GraphManagerException();
	}
			
#else
	logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "3) Flag RUN_NFS disabled. NFs will not start");
#endif
	
	/**
	*	5) Create the rules and download them in LSI-0 and tenant-LSI
	*/
	logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "5) Create the rules and download them in LSI-0 and tenant-LSI");
	try
	{
		//creates the rules for LSI-0 and for the tenant-LSI
		
		lowlevel::Graph graphLSI0 = GraphTranslator::lowerGraphToLSI0(graph,lsi,graphInfoLSI0.getLSI(), endPointsDefinedInMatches, endPointsDefinedInActions, availableEndPoints);
		logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "New graph for LSI-0:");
		graphLSI0.print();
				
		lowlevel::Graph graphTenant =  GraphTranslator::lowerGraphToTenantLSI(graph,lsi,graphInfoLSI0.getLSI());
		logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "Graph for tenant LSI:");
		graphTenant.print();	
		
		controller->installNewRules(graphTenant.getRules());

		GraphInfo graphInfoTenantLSI;
		graphInfoTenantLSI.setGraph(graph);
		graphInfoTenantLSI.setNFsManager(nfsManager);
		graphInfoTenantLSI.setLSI(lsi);
		graphInfoTenantLSI.setController(controller);

		//Save the graph information
		tenantLSIs[graph->getID()] = graphInfoTenantLSI;

		//Insert new rules into the LSI-0
		logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "Adding the new rules to the LSI-0");
		(graphInfoLSI0.getController())->installNewRules(graphLSI0.getRules());
	
		logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "Tenant LSI and its controller are created");
		
	} catch (XDPDManagerException e)
	{
#ifdef RUN_NFS
		for(map<string, list<unsigned int> >::iterator nf = network_functions.begin(); nf != network_functions.end(); nf++)
			nfsManager->stopNF(nf->first);
#endif
	
		xDPDManager.destroyLsi(*lsi);
	
		if(tenantLSIs.count(graph->getID()) != 0)
			tenantLSIs.erase(tenantLSIs.find(graph->getID()));
	
		delete(graph);
		delete(lsi);
		delete(nfsManager);
		delete(controller);

		graph = NULL;
		lsi = NULL;
		nfsManager = NULL;
		controller = NULL;

		logger(ORCH_ERROR, MODULE_NAME, __FILE__, __LINE__, "%s",e.what());
		throw GraphManagerException();
	}
	
	
	for(map<string, unsigned int >::iterator ep = availableEndPoints.begin(); ep != availableEndPoints.end(); ep++)
		logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "Endpoint \"%s\" is used %d times in graph not defining it",ep->first.c_str(),ep->second);
			
	return true;
}

bool GraphManager::updateGraph(string graphID, highlevel::Graph *newPiece)
{
	logger(ORCH_INFO, MODULE_NAME, __FILE__, __LINE__, "Updating the graph '%s'...",graphID.c_str());
	
	assert(tenantLSIs.count(graphID) != 0);

	GraphInfo graphInfo = (tenantLSIs.find(graphID))->second;
	NFsManager *nfsManager = graphInfo.getNFsManager();
	highlevel::Graph *graph = graphInfo.getGraph();
	LSI *lsi = graphInfo.getLSI();
	Controller *tenantController = graphInfo.getController();
	
	uint64_t dpid = lsi->getDpid();

	/**
	*	Outline:
	*	
	*	0) check the validity of the new piece of the graph
	*	1) update the high level graph
	*	2) select an implementation for the new NFs
	*	3) update the lsi (in case of new ports/NFs/endpoints are required)
	*	4) start the new NFs
	*	5) download the new rules in LSI-0 and tenant-LSI
	*/
	
	highlevel::Graph *tmp = new highlevel::Graph(/*"fake"*/graphID);
	
	/**
	*	0) Check the validity of the update
	*/
	logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "0) Check the validity of the update");

	//Retrieve the NFs already existing in the graph
	map<string, list<unsigned int> > nfs = graph->getNetworkFunctions();
	//Retrieve the NFs required by the update
	map<string, list<unsigned int> > new_nfs = newPiece->getNetworkFunctions();
	for(map<string, list<unsigned int> >::iterator it = new_nfs.begin(); it != new_nfs.end(); it++)
	{
		if(nfs.count(it->first) == 0)
		{
			//The NF is not part of the graph
			tmp->addNetworkFunction(it->first);
			list<unsigned int> ports = it->second;
			for(list<unsigned int>::iterator p = ports.begin(); p != ports.end(); p++)
				tmp->updateNetworkFunction(it->first,*p);
		}
		else
		{
			//The NF is already part of the graph, but the update
			//must not contain new ports for the NF
			list<unsigned int> new_ports = it->second;
			list<unsigned int> ports = nfs.find(it->first)->second;
			for(list<unsigned int>::iterator np = new_ports.begin(); np != new_ports.end(); np++)
			{
				list<unsigned int>::iterator p = ports.begin();
				for(; p != ports.end(); p++)
				{
					if(*np == *p)
						break;
				}
				if(p == ports.end())
				{
					logger(ORCH_INFO, MODULE_NAME, __FILE__, __LINE__, "A new port '%d' is required for NF '%s'",*np,it->first.c_str());
					return false;
				}
			}
		}
	}
	
	//Retrieve the ports already existing in the graph
	set<string> ports = graph->getPorts();
	//Retrieve the ports required by the update
	set<string> new_ports = newPiece->getPorts();
	for(set<string>::iterator it = new_ports.begin(); it != new_ports.end(); it++)
	{
		if(ports.count(*it) == 0)
		{
			//The physical port is not part of the graph
			tmp->addPort(*it);
		}
		else
			logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "Port %s is already in the graph",(*it).c_str());
	}
	
	//Retrieve the endpoints already existing in the graph
	set<string> endpoints = graph->getEndPoints();
	//Retrieve the endpoints required by the update
	set<string> new_endpoints = newPiece->getEndPoints();
	for(set<string>::iterator it = new_endpoints.begin(); it != new_endpoints.end(); it++)
	{
		if(endpoints.count(*it) == 0)
		{
			string tmp_ep = *it;
			string tmp_graph_id = MatchParser::graphID(tmp_ep);
			//The endpoint is not part of the graph
			tmp->addEndPoint(tmp_graph_id,*it);
		}
		else
			logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "Endpoint %s is already in the graph",(*it).c_str());
	}

	//tmp contains only the new NFs, the new ports and the new endpoints that are not already into the graph

	if(!checkGraphValidity(tmp,nfsManager))
	{
		//This is an error in the request
		delete(tmp);
		tmp = NULL;
		return false;
	}
	
	//The update is valid
	
	/**
	*	1) update the high level graph
	*/
	logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "1) Update the high level graph");
	
	list<highlevel::Rule> newRules = newPiece->getRules();
	for(list<highlevel::Rule>::iterator rule = newRules.begin(); rule != newRules.end(); rule++)
	{
		if(!graph->addRule(*rule))
		{
			logger(ORCH_INFO, MODULE_NAME, __FILE__, __LINE__, "The graph has at least two rules with the same ID: %s",rule->getFlowID().c_str());
			return false;
		}
	}
	set<string> nps = tmp->getPorts();
	for(set<string>::iterator port = nps.begin(); port != nps.end(); port++)
		graph->addPort(*port);
	map<string, list<unsigned int> > networkFunctions = tmp->getNetworkFunctions();
	for(map<string, list<unsigned int> >::iterator nf = networkFunctions.begin(); nf != networkFunctions.end(); nf++)
	{
		graph->addNetworkFunction(nf->first);
		list<unsigned int> nfPorts = nf->second;
		for(list<unsigned int>::iterator p = nfPorts.begin(); p != nfPorts.end(); p++)
		{
			graph->updateNetworkFunction(nf->first,*p);
		}
	}
	set<string> nep = tmp->getEndPoints();
	for(set<string>::iterator ep = nep.begin(); ep != nep.end(); ep++)
	{
		string tmp_ep = *ep;
		string tmp_graph_id = MatchParser::graphID(tmp_ep);
		//The endpoint is not part of the graph
		graph->addEndPoint(tmp_graph_id,*ep);
	}
	
	graph->print();
	
	/**
	*	2) Select an implementation for the new NFs
	*/
	logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "2) Select an implementation for the new NFs");	
	if(!nfsManager->selectImplementation())
	{
		//This is an internal error
		delete(nfsManager);
		nfsManager = NULL;
		throw GraphManagerException();
	}
	
	/**
	*	3) Update the lsi (in case of new ports/NFs/endpoints are required)
	*/
	logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "3) update the lsi (in case of new ports/NFs/endpoints are required)");
	
	set<string> phyPorts = tmp->getPorts();
	map<string, list<unsigned int> > network_functions = tmp->getNetworkFunctions();
	
	//Since the NFs cannot specify new ports, new virtual links can be required only by the new NFs and the physical ports
	
	vector<set<string> > vlVector = identifyVirtualLinksRequired(newPiece,lsi);
	set<string> vlNFs = vlVector[0];
	set<string> vlPhyPorts = vlVector[1];
	set<string> vlEndPoints = vlVector[2];
	set<string> NFsFromEndPoint = vlVector[3];

	//TODO: check if a virtual link is already available and can be used (because it is currentl used only in one direction)	
	int numberOfVLrequiredBeforeEndPoints = (vlNFs.size() > vlPhyPorts.size())? vlNFs.size() : vlPhyPorts.size();
	unsigned int numberOfVLrequired = numberOfVLrequiredBeforeEndPoints + vlEndPoints.size();
	
	logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "%d virtual links are required to connect the new part of the LSI with LSI-0",numberOfVLrequired);

	try
	{
		set<string>::iterator nf = vlNFs.begin();
		set<string>::iterator p = vlPhyPorts.begin();
		for(; nf != vlNFs.end() || p != vlPhyPorts.end() ;)
		{
		
			uint64_t vlinkID = xDPDManager.addVirtualLink(*lsi,VLink(dpid0));
			
			VLink vlink = lsi->getVirtualLink(vlinkID);
			logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "Virtual link: (ID: %x) %x:%d -> %x:%d",vlink.getID(),dpid,vlink.getLocalID(),vlink.getRemoteDpid(),vlink.getRemoteID());
	
			if(nf != vlNFs.end())
			{
				lsi->addNFvlink(*nf,vlinkID);
				logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "NF '%s' uses the vlink '%x'",(*nf).c_str(),vlink.getID());
				nf++;
			}
			if(p != vlPhyPorts.end())
			{
				lsi->addPortvlink(*p,vlinkID);
				logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "Physical port '%s' uses the vlink '%x'",(*p).c_str(),vlink.getID());
				p++;
			}
		}
	
		for(set<string>::iterator ep = vlEndPoints.begin(); ep != vlEndPoints.end(); ep++)
		{
			uint64_t vlinkID = xDPDManager.addVirtualLink(*lsi,VLink(dpid0));
	
			VLink vlink = lsi->getVirtualLink(vlinkID);
			logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "Virtual link: (ID: %x) %x:%d -> %x:%d",vlink.getID(),dpid,vlink.getLocalID(),vlink.getRemoteDpid(),vlink.getRemoteID());

			lsi->addEndpointvlink(*ep,vlinkID);
			logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "Endpoint '%s' uses the vlink '%x'",(*ep).c_str(),vlink.getID());
			
			if(graph->isDefinedHere(*ep))
			{
				//since this endpoint is in an action (hence it requires a virtual link), and it is defined in this
				//graph, we save the port of the vlink in LSI-0, so that other graphs can use this endpoint
				//Since we are considering this endpoint now, it means that is defined for the first time in this update
				//of the graph.
					
				logger(ORCH_INFO, MODULE_NAME, __FILE__, __LINE__, "The endpoint \"%s\" is defined in an action of the current Graph. Other graph can use it expressing a match on the port %d of the LSI-0",(*ep).c_str(),vlink.getRemoteID());
				endPointsDefinedInActions[*ep] = vlink.getRemoteID();
			
				//This endpoint is currently not used in any other graph, since it is defined in the current graph
				availableEndPoints[*ep] = 0; 
			}
			
		}
	}catch(XDPDManagerException e)
	{
		logger(ORCH_ERROR, MODULE_NAME, __FILE__, __LINE__, "%s",e.what());
		delete(tmp);
		tmp = NULL;	
		throw GraphManagerException();
	}
	
	for(set<string>::iterator nf = NFsFromEndPoint.begin(); nf != NFsFromEndPoint.end(); nf++)
	{
		//XXX: this works because I'm assuming that a graph cannot use twice the same endpoint in matches, if this
		//endpoint is defined by the graph itself.
	
		//since this rule has a graph endpoint in the match that is defined in this
		//graph, we save the port of the vlink of the NF in LSI-0, so that other graphs can use this endpoint
		string ep = findEndPointTowardsNF(newPiece,*nf);
		
		map<string, uint64_t> nfs_vlinks = lsi->getNFsVlinks();
		assert(nfs_vlinks.count(*nf) != 0);
		
		VLink vlink = lsi->getVirtualLink(nfs_vlinks.find(*nf)->second);
		
		logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "The endpoint \"%s\" is defined in a match of the current Graph. Other graphs can use it expressing an action on the port %d of the LSI-0",ep.c_str(),vlink.getRemoteID());
		endPointsDefinedInMatches[ep] = vlink.getRemoteID();

		//This endpoint is currently not used in any other graph, since it is defined in the current graph
		availableEndPoints[ep] = 0; 
	}


	for(map<string, list<unsigned int> >::iterator nf = network_functions.begin(); nf != network_functions.end(); nf++)
	{
		try
		{
			xDPDManager.addNFPorts(*lsi,*nf,nfsManager->getNFType(nf->first));
		}catch(XDPDManagerException e)
		{
			logger(ORCH_ERROR, MODULE_NAME, __FILE__, __LINE__, "%s",e.what());
			delete(tmp);
			tmp = NULL;	
			throw GraphManagerException();
		}
	}
		
	/**
	*	4) Start the new NFs
	*/
#ifdef RUN_NFS
	logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "4) start the new NFs");
	
	nfsManager->setLsiID(dpid);
	
	for(map<string, list<unsigned int> >::iterator nf = network_functions.begin(); nf != network_functions.end(); nf++)
	{
		if(!nfsManager->startNF(nf->first, nf->second.size(),newPiece->getNetworkFunctionIPv4PortsRequirements(nf->first),newPiece->getNetworkFunctionEthernetPortsRequirements(nf->first)))
		{
			//TODO: no idea on what I have to do at this point
			assert(0);
			delete(tmp);
			tmp = NULL;
			throw GraphManagerException();
		}
	}
#else
	logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "4) Flag RUN_NFS disabled. New NFs will not start");
#endif

	/**
	*	5) Create the new rules and download them in LSI-0 and tenant-LSI
	*/
	logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "5) Create the new rules and download them in LSI-0 and tenant-LSI");

	try
	{
		//creates the new rules for LSI-0 and for the tenant-LSI
		
		lowlevel::Graph graphLSI0 = GraphTranslator::lowerGraphToLSI0(newPiece,lsi,graphInfoLSI0.getLSI(), endPointsDefinedInMatches, endPointsDefinedInActions, availableEndPoints);
		logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "New piece of graph for LSI-0:");
		graphLSI0.print();
				
		lowlevel::Graph graphTenant =  GraphTranslator::lowerGraphToTenantLSI(newPiece,lsi,graphInfoLSI0.getLSI());
		logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "New piece of graph for tenant LSI:");
		graphTenant.print();	

		//Insert new rules into the LSI-0
		logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "Adding the new rules to the LSI-0");
		(graphInfoLSI0.getController())->installNewRules(graphLSI0.getRules());
	
		//Insert new rules into the tenant-LSI
		logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "Adding the new rules to the tenant-LSI");
		tenantController->installNewRules(graphTenant.getRules());
		
	} catch (XDPDManagerException e)
	{
		//TODO: no idea on what I have to do at this point
		assert(0);
		delete(tmp);
		tmp = NULL;
		throw GraphManagerException();
	
		logger(ORCH_ERROR, MODULE_NAME, __FILE__, __LINE__, "%s",e.what());
		throw GraphManagerException();
	}

	//The new flows have been added to the graph!
	
	delete(tmp);
	tmp = NULL;
	return true;
}

vector<set<string> > GraphManager::identifyVirtualLinksRequired(highlevel::Graph *graph)
{
	set<string> NFs;
	set<string> phyPorts;
	set<string> endPoints;
	
	set<string> NFsFromEndPoint;
	
	list<highlevel::Rule> rules = graph->getRules();
	for(list<highlevel::Rule>::iterator rule = rules.begin(); rule != rules.end(); rule++)
	{
		highlevel::Action *action = rule->getAction();
		highlevel::Match match = rule->getMatch();
		if(action->getType() == highlevel::ACTION_ON_NETWORK_FUNCTION)
		{
			if(match.matchOnPort() || match.matchOnEndPoint())
			{
				highlevel::ActionNetworkFunction *action_nf = (highlevel::ActionNetworkFunction*)action;
				stringstream ss;
				ss << action->getInfo() << "_" << action_nf->getPort();
				NFs.insert(ss.str());
				
				if(match.matchOnEndPoint())
				{
					stringstream ssm;
					ssm << match.getGraphID() << ":" << match.getEndPoint();
					if(graph->isDefinedHere(ssm.str()))
						NFsFromEndPoint.insert(ss.str());
				}
			}
		}
		else if(action->getType() == highlevel::ACTION_ON_PORT)
		{
			if(match.matchOnNF())
				phyPorts.insert(action->getInfo());
		}
		else if(action->getType() == highlevel::ACTION_ON_ENDPOINT)
		{
			assert(match.matchOnNF());
			highlevel::ActionEndPoint *action_ep = (highlevel::ActionEndPoint*)action;
			//stringstream ss;
			//ss << action_ep->getInfo() << ":" << action_ep->getPort();			
			//endPoints.insert(ss.str());
			endPoints.insert(action_ep->toString());
		}
	}
	
	logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "Network functions input ports requiring a virtual link:");
	for(set<string>::iterator nf = NFs.begin(); nf != NFs.end(); nf++)
		logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "\t%s",(*nf).c_str());
	logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "Physical ports requiring a virtual link:");
	for(set<string>::iterator p = phyPorts.begin(); p != phyPorts.end(); p++)
		logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "\t%s",(*p).c_str());
	logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "Endpoints requiring a virtual link:");
	for(set<string>::iterator e = endPoints.begin(); e != endPoints.end(); e++)
		logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "\t%s",(*e).c_str());
	logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "NFs reached from an endpoint defined in this graph:");
	for(set<string>::iterator nfe = NFsFromEndPoint.begin(); nfe != NFsFromEndPoint.end(); nfe++)
		logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "\t%s",(*nfe).c_str());	
	
	vector<set<string> > retval;
	vector<set<string> >::iterator rv;
	
	rv = retval.end();
	retval.insert(rv,NFs);
	rv = retval.end();
	retval.insert(rv,phyPorts);
	rv = retval.end();
	retval.insert(rv,endPoints);
	rv = retval.end();
	retval.insert(rv,NFsFromEndPoint);
	
	return retval;
}

vector<set<string> > GraphManager::identifyVirtualLinksRequired(highlevel::Graph *newPiece, LSI *lsi)
{
	set<string> NFs;
	set<string> phyPorts;
	set<string> endPoints;

	set<string> NFsFromEndPoint;	

	list<highlevel::Rule> rules = newPiece->getRules();
	for(list<highlevel::Rule>::iterator rule = rules.begin(); rule != rules.end(); rule++)
	{
		highlevel::Action *action = rule->getAction();
		highlevel::Match match = rule->getMatch();
		if(action->getType() == highlevel::ACTION_ON_NETWORK_FUNCTION)
		{
			if(match.matchOnPort() || match.matchOnEndPoint())
			{
				highlevel::ActionNetworkFunction *action_nf = (highlevel::ActionNetworkFunction*)action;

				//Check if a vlink is required for this NF port
				map<string, uint64_t> nfs_vlinks = lsi->getNFsVlinks();
				stringstream ss;
				ss << action->getInfo() << "_" << action_nf->getPort();
				if(nfs_vlinks.count(ss.str()) == 0)
					NFs.insert(ss.str());
					
				if(match.matchOnEndPoint())
				{
					stringstream ssm;
					ssm << match.getGraphID() << ":" << match.getEndPoint();
					if(newPiece->isDefinedHere(ssm.str()))
						NFsFromEndPoint.insert(ss.str());
				}
			}
		}
		else if(action->getType() == highlevel::ACTION_ON_PORT)
		{
			//check if a vlink is required for this physical port
			map<string, uint64_t> ports_vlinks = lsi->getPortsVlinks();
			highlevel::ActionPort *action_port = (highlevel::ActionPort*)action;
			if(ports_vlinks.count(action_port->getInfo()) == 0)
				phyPorts.insert(action_port->getInfo());
		}
		else if(action->getType() == highlevel::ACTION_ON_ENDPOINT)
		{
			assert(match.matchOnNF());
			
			//check if a vlink is required for this endpoint
			map<string, uint64_t> endpoints_vlinks = lsi->getEndPointsVlinks();
			highlevel::ActionEndPoint *action_ep = (highlevel::ActionEndPoint*)action;
			if(endpoints_vlinks.count(action_ep->toString()) == 0)
				endPoints.insert(action_ep->toString());
		}
	}
	
	logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "Network functions input ports requiring a virtual link:");
	for(set<string>::iterator nf = NFs.begin(); nf != NFs.end(); nf++)
		logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "\t%s",(*nf).c_str());
	logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "Physical ports requiring a virtual link:");
	for(set<string>::iterator p = phyPorts.begin(); p != phyPorts.end(); p++)
		logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "\t%s",(*p).c_str());
	logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "Endpoints requiring a virtual link:");
	for(set<string>::iterator e = endPoints.begin(); e != endPoints.end(); e++)
		logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "\t%s",(*e).c_str());
	logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "NFs reached from an endpoint defined in this graph:");
	for(set<string>::iterator nfe = NFsFromEndPoint.begin(); nfe != NFsFromEndPoint.end(); nfe++)
		logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "\t%s",(*nfe).c_str());	
	
	//prepare the return value
	vector<set<string> > retval;
	vector<set<string> >::iterator rv;
	rv = retval.end();
	retval.insert(rv,NFs);
	rv = retval.end();
	retval.insert(rv,phyPorts);
	rv = retval.end();
	retval.insert(rv,endPoints);
	rv = retval.end();
	retval.insert(rv,NFsFromEndPoint);
	
	return retval;
}

void GraphManager::removeUselessPorts_NFs_Endpoints_VirtualLinks(RuleRemovedInfo rri, NFsManager *nfsManager,highlevel::Graph *graph, LSI * lsi)
{
	map<string, uint64_t> nfs_vlinks = lsi->getNFsVlinks();
	map<string, uint64_t> ports_vlinks = lsi->getPortsVlinks();
	map<string, uint64_t> endpoints_vlinks = lsi->getEndPointsVlinks();
	
	
	list<highlevel::Rule> rules = graph->getRules();
	
	if(rri.isNFport)
		logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "Check if the vlink associated with the NF port '%s' must be removed (if this vlink exists)",rri.nf_port.c_str());
	
	if(rri.isNFport && nfs_vlinks.count(rri.nf_port) != 0)
	{
		logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "The NF port '%s' is associated with a vlink",rri.nf_port.c_str());
		
		/**
		*	In case NF:port does not appear in other actions, the vlink must be removed
		*/
		bool equal = false;
		for(list<highlevel::Rule>::iterator again = rules.begin(); again != rules.end(); again++)
		{
		
			highlevel::Action *a = again->getAction();
			if(a->getType() == highlevel::ACTION_ON_NETWORK_FUNCTION)
			{
				if(((highlevel::ActionNetworkFunction*)a)->getInfo() == rri.nf_port)
				{
					//The action is on the same NF:port of the removed one, hence
					//the vlink must not be removed
					equal = true;
					break;
				}
			}
			
		}//end of again iterator on the rules of the graph		
		if(!equal)
		{
			//We just know that the vlink is no longer used for a NF. However, it might used in the opposite
			//direction, for a port
			logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "Virtual link no longer required for NF port: %s",rri.nf_port.c_str());
			uint64_t nfvlink = nfs_vlinks.find(rri.nf_port)->second;
			
			lsi->removeNFvlink(rri.nf_port);
			
			for(map<string, uint64_t>::iterator pvl = ports_vlinks.begin(); pvl != ports_vlinks.end(); pvl++)
			{
				if(pvl->second == nfvlink)
				{
					logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "The virtual link cannot be removed because it is still used by the port: %s",pvl->first.c_str());
					goto next;
				}
			}
			
			logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "The virtual link must be removed");
			
			try
			{
				xDPDManager.destroyVirtualLink(*lsi,nfvlink);
			} catch (XDPDManagerException e)
			{
				logger(ORCH_ERROR, MODULE_NAME, __FILE__, __LINE__, "%s",e.what());
				throw GraphManagerException();
			}
		}
	}	

next:	
	
	if(rri.isPort)
		logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "Check of the vlink associated with the port '%s' must be removed (if this vlink exists)",rri.port.c_str());
	
	if(rri.isPort && ports_vlinks.count(rri.port) != 0)
	{
		logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "The port '%s' is associated with a vlink",rri.port.c_str());
		/**
		*	In case port does not appear in other actions, the vlink must be removed
		*/
		bool equal = false;
		for(list<highlevel::Rule>::iterator again = rules.begin(); again != rules.end(); again++)
		{	
			highlevel::Action *a = again->getAction();
			if(a->getType() == highlevel::ACTION_ON_PORT)
			{
				if(((highlevel::ActionPort*)a)->getInfo() == rri.port)
				{
					//The action are the same, hence no vlink must be removed
					equal = true;
					break;
				}
			}
			
		}//end of again iterator on the rules of the graph		
		if(!equal)
		{
			//We just know that the vlink is no longer used for a port. However, it might used in the opposite
			//direction, for a NF port
			logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "Virtual link no longer required for port: %s",rri.port.c_str());
			uint64_t portlink = ports_vlinks.find(rri.port)->second;
			
			lsi->removePortvlink(rri.port);
			
			for(map<string, uint64_t>::iterator nfvl = nfs_vlinks.begin(); nfvl != nfs_vlinks.end(); nfvl++)
			{
				if(nfvl->second == portlink)
				{
					logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "The virtual link cannot be removed because it is still used by the NF port: %s",nfvl->first.c_str());
					goto next2;
				}
			}
			
			logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "The virtual link must be removed");
			try
			{
				xDPDManager.destroyVirtualLink(*lsi,portlink);
			} catch (XDPDManagerException e)
			{
				logger(ORCH_ERROR, MODULE_NAME, __FILE__, __LINE__, "%s",e.what());
				throw GraphManagerException();
			}
		}
	}
	
next2:

	if(rri.isEndpoint)
		logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "Check if the vlink associated with the endpoint '%s' must be removed (if this vlink exists)",rri.endpoint.c_str());
	
	if(rri.isEndpoint && endpoints_vlinks.count(rri.endpoint) != 0)
	{
		logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "The endpoint '%s' is associated with a vlink",rri.endpoint.c_str());
		
		/**
		*	In case the endpoint does not appear in other actions, the vlink must be removed
		*/
		bool equal = false;
		for(list<highlevel::Rule>::iterator again = rules.begin(); again != rules.end(); again++)
		{
		
			highlevel::Action *a = again->getAction();
			if(a->getType() == highlevel::ACTION_ON_ENDPOINT)
			{
				if(((highlevel::ActionEndPoint*)a)->toString() == rri.endpoint)
				{
					//The action is on the same endpoint of the removed one, hence
					//the vlink must not be removed
					equal = true;
					break;
				}
			}
			
		}//end of again iterator on the rules of the graph		
		if(!equal)
		{
			logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "Virtual link no longer required for the endpoint: %s",rri.endpoint.c_str());
			
			uint64_t epvlink = endpoints_vlinks.find(rri.endpoint)->second;			
			lsi->removeEndPointvlink(rri.endpoint);
			
			logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "The virtual link must be removed");
			
			try
			{
				xDPDManager.destroyVirtualLink(*lsi,epvlink);
			} catch (XDPDManagerException e)
			{
				logger(ORCH_ERROR, MODULE_NAME, __FILE__, __LINE__, "%s",e.what());
				throw GraphManagerException();
			}
		}
	}

	//Remove NFs, if they no longer appear in the graph
	for(list<string>::iterator nf = rri.nfs.begin(); nf != rri.nfs.end(); nf++)
	{
		if(!graph->stillExistNF(*nf))
		{
			logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "The NF '%s' is no longer part of the graph",(*nf).c_str());

			//Stop the NF	
#ifdef RUN_NFS
			nfsManager->stopNF(*nf);
#else
			logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "Flag RUN_NFS disabled. No NF to be stopped");
#endif
			try
			{
				xDPDManager.destroyNFPorts(*lsi,*nf);
			} catch (XDPDManagerException e)
			{
				logger(ORCH_ERROR, MODULE_NAME, __FILE__, __LINE__, "%s",e.what());
				throw GraphManagerException();
			}
		}
	}
	
	//Remove physical ports, if they no longer appear in the graph
	for(list<string>::iterator p = rri.ports.begin(); p != rri.ports.end(); p++)
	{
		if(!graph->stillExistPort(*p))
			logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "The port '%s' is no longer part of the graph",(*p).c_str());
	}
	
	//Remove the endpoint, if it no longer appear in the graph
	if((rri.endpoint != "") && (!graph->stillExistEndpoint(rri.endpoint)))
		logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "The endpoint '%s' is no longer part of the graph",rri.endpoint.c_str());	
}

bool GraphManager::attachWirelessPort(LSI *lsi)
{
	//The interface created by xDPd for the wireless, must be attached to the real wireles interface through a bridge

	logger(ORCH_INFO, MODULE_NAME, __FILE__, __LINE__, "Attaching the wireless interface '%s'...",lsi->getWirelessPortName().c_str());
	
	stringstream command;
	command << ATTACH_WIRELESS_INTERFACE << " " << lsi->getDpid() << " " << lsi->getWirelessPortName();
	logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "Executing command \"%s\"",command.str().c_str());

	int retVal = system(command.str().c_str());
	retVal = retVal >> 8;
	
	if(retVal == 0)
		return false;
	else
		return true;
}

void GraphManager::detachWirelessPort(LSI *lsi)
{
	if(lsi->hasWireless())
	{
		logger(ORCH_INFO, MODULE_NAME, __FILE__, __LINE__, "Detaching the wireless interface '%s'...",lsi->getWirelessPortName().c_str());
		stringstream command;
		command << DETACH_WIRELESS_INTERFACE << " " << lsi->getDpid() << " " << lsi->getWirelessPortName();
		logger(ORCH_DEBUG_INFO, MODULE_NAME, __FILE__, __LINE__, "Executing command \"%s\"",command.str().c_str());

		int retVal = system(command.str().c_str());
		retVal += 1; //XXX: just to remove a warning
	}
}

string GraphManager::findEndPointTowardsNF(highlevel::Graph *graph, string nf)
{
	list<highlevel::Rule> rules = graph->getRules();
	for(list<highlevel::Rule>::iterator rule = rules.begin(); rule != rules.end(); rule++)
	{
		highlevel::Action *action = rule->getAction();
		highlevel::Match match = rule->getMatch();
		if(action->getType() == highlevel::ACTION_ON_NETWORK_FUNCTION && match.matchOnEndPoint())
		{
			highlevel::ActionNetworkFunction *action_nf = (highlevel::ActionNetworkFunction*)action;
			stringstream ss;
			ss << action->getInfo() << "_" << action_nf->getPort();
			
			if(nf == ss.str())
			{
				stringstream ssm;
				ssm << match.getGraphID() << ":" << match.getEndPoint();
				return ssm.str();
			}		
		}
	}

	assert(0);
	
	return ""; //just for the compiler
}

bool GraphManager::canDeleteFlow(highlevel::Graph *graph, string flowID)
{
	highlevel::Rule r = graph->getRuleFromID(flowID);
	highlevel::Match m = r.getMatch();
	highlevel::Action *a = r.getAction();
	
	set<string> endpoints = graph->getEndPoints();
	for(set<string>::iterator ep = endpoints.begin(); ep != endpoints.end(); ep++)
	{
		if(graph->isDefinedHere(*ep))
		{
			if( (a->getType() == highlevel::ACTION_ON_ENDPOINT) && (a->toString() == *ep) )
			{
				if(availableEndPoints.find(*ep)->second !=0)
				{
					logger(ORCH_INFO, MODULE_NAME, __FILE__, __LINE__, "The flow cannot be deleted. It defines (in the action) the endpoint \"%s\" that is used %d times in other graphs; first remove the rules in those graphs.",ep->c_str(),availableEndPoints.find(*ep)->second);
					return false;
				}			
			}
			if(m.matchOnEndPoint())
			{
				stringstream ss;
				ss << m.getGraphID() << ":" << m.getEndPoint();
				if(ss.str() == *ep && availableEndPoints.find(*ep)->second !=0)
				{
					logger(ORCH_INFO, MODULE_NAME, __FILE__, __LINE__, "The flow cannot be deleted. It defines (in the match) the endpoint \"%s\" that is used %d times in other graphs; first remove the rules in those graphs.",ep->c_str(),availableEndPoints.find(*ep)->second);
					return false;
				}
			}
		}
	}
	
	return true;
}

